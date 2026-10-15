#pragma once

#include <QString>
#include <string>
#include <utility>
#include <vector>

// Big screen settings with a few named values, as (value, label), for big screen's own settings
// and the desktop's.
namespace mira_gui::bigscreen {

inline const std::vector<std::pair<std::string, QString>> kButtonKinds = {
    {"auto", "Match controller"}, {"xbox", "Xbox"}, {"ps", "PlayStation"}, {"nin", "Nintendo"}};
inline const std::vector<std::pair<std::string, QString>> kStick = {
    {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}};
inline const std::vector<std::pair<std::string, QString>> kRepeat = {
    {"slow", "Slow"}, {"normal", "Normal"}, {"fast", "Fast"}};
inline const std::vector<std::pair<std::string, QString>> kRumble = {
    {"off", "Off"}, {"light", "Light"}, {"strong", "Strong"}};
inline const std::vector<std::pair<int, QString>> kIdle = {
    {0, "Never"}, {15, "15 minutes"}, {30, "30 minutes"}, {60, "1 hour"}};

}  // namespace mira_gui::bigscreen
