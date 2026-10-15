#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace mira_gui {

// Where mirad says the user can fix an error (docs/api.md, "Errors").
struct ErrorFix {
  std::string kind;  // "setting", "runners", "source", "game"; empty for none
  std::string target;
  std::string step;
};

// A failed request: mirad's message, code, hint and fix, so the UI can show
// what to do and offer a button for it (ui/ErrorHelp). Reads as the message
// wherever a plain string is expected.
struct ApiError {
  // Set by the transport when the request never reached mirad.
  static constexpr const char* kUnreachable = "mirad_unreachable";

  std::string message;
  std::string code;  // empty for a failure raised by the client itself
  std::string hint;
  ErrorFix fix;

  ApiError() = default;
  ApiError(std::string text) : message(std::move(text)) {}  // NOLINT: implicit, a client-side error
  ApiError(const char* text) : message(text) {}             // NOLINT
  ApiError(std::string text, std::string error_code)
      : message(std::move(text)), code(std::move(error_code)) {}

  operator const std::string&() const { return message; }  // NOLINT: reads as its message
  bool empty() const { return message.empty(); }
  bool operator==(std::string_view text) const { return message == text; }
  const char* c_str() const { return message.c_str(); }
};

}  // namespace mira_gui
