/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup glsl_preprocess
 */

#pragma once

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace blender::gpu::shader {

/**
 * Shader source preprocessor that allow to mutate GLSL into cross API source that can be
 * interpreted by the different GPU backends. Some syntax are mutated or reported as incompatible.
 *
 * Implementation speed is not a huge concern as we only apply this at compile time or on python
 * shaders source.
 */
class Preprocessor {
  using uint = unsigned int;

  struct SharedVar {
    std::string type;
    std::string name;
    std::string array;
  };
  std::vector<SharedVar> shared_vars_;

  std::stringstream output_;

 public:
  /* Takes a whole source file and output processed source. */
  template<typename ReportErrorF>
  std::string process(std::string str,
                      bool /*do_linting*/,
                      bool /*do_string_mutation*/,
                      bool /*do_include_mutation*/,
                      const ReportErrorF &report_error)
  {
    str = remove_comments(str, report_error);
    threadgroup_variable_parsing(str);
    matrix_constructor_linting(str, report_error);
    array_constructor_linting(str, report_error);
    str = preprocessor_directive_mutation(str);
    str = argument_decorator_macro_injection(str);
    str = array_constructor_macro_injection(str);
    return str + suffix();
  }

  /* Variant use for python shaders. */
  std::string process(const std::string &str)
  {
    auto no_err_report = [](std::string, std::smatch, const char *) {};
    return process(str, false, false, false, no_err_report);
  }

 private:
  template<typename ReportErrorF>
  std::string remove_comments(const std::string &str, const ReportErrorF &report_error)
  {
    std::string out_str = str;
    {
      /* Multi-line comments. */
      size_t start, end = 0;
      while ((start = out_str.find("/*", end)) != std::string::npos) {
        end = out_str.find("*/", start + 2);
        if (end == std::string::npos) {
          break;
        }
        for (size_t i = start; i < end + 2; ++i) {
          if (out_str[i] != '\n') {
            out_str[i] = ' ';
          }
        }
      }

      if (end == std::string::npos) {
        /* TODO(fclem): Add line / char position to report. */
        report_error(str, std::smatch(), "Malformed multi-line comment.");
        return out_str;
      }
    }
    {
      /* Single-line comments. */
      size_t start, end = 0;
      while ((start = out_str.find("//", end)) != std::string::npos) {
        end = out_str.find('\n', start + 2);
        if (end == std::string::npos) {
          break;
        }
        for (size_t i = start; i < end; ++i) {
          out_str[i] = ' ';
        }
      }

      if (end == std::string::npos) {
        /* TODO(fclem): Add line / char position to report. */
        report_error(str, std::smatch(), "Malformed single line comment, missing newline.");
        return out_str;
      }
    }
    /* Remove trailing whitespaces as they make the subsequent regex much slower. */
    std::regex regex("(\\ )*?\\n");
    return std::regex_replace(out_str, regex, "\n");
  }

  std::string preprocessor_directive_mutation(const std::string &str)
  {
    /* Example: `#include "deps.glsl"` > `//include "deps.glsl"` */
    std::regex regex("#\\s*(include|pragma once)");
    return std::regex_replace(str, regex, "//$1");
  }

  void threadgroup_variable_parsing(std::string str)
  {
    std::regex regex("shared\\s+(\\w+)\\s+(\\w+)([^;]*);");
    for (std::smatch match; std::regex_search(str, match, regex); str = match.suffix()) {
      shared_vars_.push_back({match[1].str(), match[2].str(), match[3].str()});
    }
  }

  std::string argument_decorator_macro_injection(const std::string &str)
  {
    /* Example: `out float var[2]` > `out float _out_sta var _out_end[2]` */
    std::regex regex("(out|inout|in|shared)\\s+(\\w+)\\s+(\\w+)");
    return std::regex_replace(str, regex, "$1 $2 _$1_sta $3 _$1_end");
  }

  std::string array_constructor_macro_injection(const std::string &str)
  {
    /* Example: `= float[2](0.0, 0.0)` > `= ARRAY_T(float) ARRAY_V(0.0, 0.0)` */
    std::regex regex("=\\s*(\\w+)\\s*\\[[^\\]]*\\]\\s*\\(");
    return std::regex_replace(str, regex, "= ARRAY_T($1) ARRAY_V(");
  }

  /* TODO(fclem): Too many false positive and false negative to be applied to python shaders. */
  template<typename ReportErrorF>
  void matrix_constructor_linting(std::string str, const ReportErrorF &report_error)
  {
    /* Example: `mat4(other_mat)`. */
    std::regex regex("\\s+(mat(\\d|\\dx\\d)|float\\dx\\d)\\([^,\\s\\d]+\\)");
    for (std::smatch match; std::regex_search(str, match, regex); str = match.suffix()) {
      /* This only catches some invalid usage. For the rest, the CI will catch them. */
      const char *msg =
          "Matrix constructor is not cross API compatible. "
          "Use to_floatNxM to reshape the matrix or use other constructors instead.";
      report_error(str, match, msg);
    }
  }

  template<typename ReportErrorF>
  void array_constructor_linting(std::string str, const ReportErrorF &report_error)
  {
    std::regex regex("=\\s*(\\w+)\\s*\\[[^\\]]*\\]\\s*\\(");
    for (std::smatch match; std::regex_search(str, match, regex); str = match.suffix()) {
      /* This only catches some invalid usage. For the rest, the CI will catch them. */
      const char *msg =
          "Array constructor is not cross API compatible. Use type_array instead of type[].";
      report_error(str, match, msg);
    }
  }

  std::string suffix()
  {
    if (shared_vars_.empty()) {
      return "";
    }

    std::stringstream suffix;
    /**
     * For Metal shaders to compile, shared (threadgroup) variable cannot be declared globally.
     * They must reside within a function scope. Hence, we need to extract these declarations and
     * generate shared memory blocks within the entry point function. These shared memory blocks
     * can then be passed as references to the remaining shader via the class function scope.
     *
     * The shared variable definitions from the source file are replaced with references to
     * threadgroup memory blocks (using _shared_sta and _shared_end macros), but kept in-line in
     * case external macros are used to declare the dimensions.
     *
     * Each part of the codegen is stored inside macros so that we don't have to do string
     * replacement at runtime.
     */
    /* Arguments of the wrapper class constructor. */
    suffix << "#undef MSL_SHARED_VARS_ARGS\n";
    /* References assignment inside wrapper class constructor. */
    suffix << "#undef MSL_SHARED_VARS_ASSIGN\n";
    /* Declaration of threadgroup variables in entry point function. */
    suffix << "#undef MSL_SHARED_VARS_DECLARE\n";
    /* Arguments for wrapper class constructor call. */
    suffix << "#undef MSL_SHARED_VARS_PASS\n";

    /**
     * Example replacement:
     *
     * `
     * // Source
     * shared float bar[10];                                    // Source declaration.
     * shared float foo;                                        // Source declaration.
     * // Rest of the source ...
     * // End of Source
     *
     * // Backend Output
     * class Wrapper {                                          // Added at runtime by backend.
     *
     * threadgroup float (&foo);                                // Replaced by regex and macros.
     * threadgroup float (&bar)[10];                            // Replaced by regex and macros.
     * // Rest of the source ...
     *
     * Wrapper (                                                // Added at runtime by backend.
     * threadgroup float (&_foo), threadgroup float (&_bar)[10] // MSL_SHARED_VARS_ARGS
     * )                                                        // Added at runtime by backend.
     * : foo(_foo), bar(_bar)                                   // MSL_SHARED_VARS_ASSIGN
     * {}                                                       // Added at runtime by backend.
     *
     * }; // End of Wrapper                                     // Added at runtime by backend.
     *
     * kernel entry_point() {                                   // Added at runtime by backend.
     *
     * threadgroup float foo;                                   // MSL_SHARED_VARS_DECLARE
     * threadgroup float bar[10]                                // MSL_SHARED_VARS_DECLARE
     *
     * Wrapper wrapper                                          // Added at runtime by backend.
     * (foo, bar)                                               // MSL_SHARED_VARS_PASS
     * ;                                                        // Added at runtime by backend.
     *
     * }                                                        // Added at runtime by backend.
     * // End of Backend Output
     * `
     */
    std::stringstream args, assign, declare, pass;

    bool first = true;
    for (SharedVar &var : shared_vars_) {
      char sep = first ? ' ' : ',';
      /*  */
      args << sep << "threadgroup " << var.type << "(&_" << var.name << ")" << var.array;
      assign << (first ? ':' : ',') << var.name << "(_" << var.name << ")";
      declare << "threadgroup " << var.type << ' ' << var.name << var.array << ";";
      pass << sep << var.name;
      first = false;
    }

    suffix << "#define MSL_SHARED_VARS_ARGS " << args.str() << "\n";
    suffix << "#define MSL_SHARED_VARS_ASSIGN " << assign.str() << "\n";
    suffix << "#define MSL_SHARED_VARS_DECLARE " << declare.str() << "\n";
    suffix << "#define MSL_SHARED_VARS_PASS (" << pass.str() << ")\n";

    return suffix.str();
  }
};

}  // namespace blender::gpu::shader
