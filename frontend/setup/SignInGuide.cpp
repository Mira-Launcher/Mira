#include "SignInGuide.h"

namespace mira_gui {
namespace {

using Kind = SketchPart::Kind;

SketchPart Part(Kind kind, const QString& text, bool marked = false, int step = 0) {
  return {kind, {text}, marked, step};
}

const QString kCopyAddress =
    "Copy the address from the address bar: <code>Ctrl+L</code>, then <code>Ctrl+C</code>.";

Sketch Cookies(bool firefox) {
  Sketch sketch;
  sketch.address = "www.humblebundle.com/home/library";
  sketch.parts = {Part(Kind::Heading, "Your Library")};
  SketchPart tabs{Kind::Tabs,
                  firefox ? QStringList{"Inspector", "Console", "Network", "Storage"}
                          : QStringList{"Elements", "Console", "Network", "Application"},
                  false, 0, 3};
  sketch.parts.push_back(tabs);
  sketch.parts.push_back(Part(Kind::Text, "Cookies  >  https://www.humblebundle.com"));
  sketch.parts.push_back({Kind::TableRow, {"csrf_cookie", "rT9xW2a1qLmZ"}});
  sketch.parts.push_back({Kind::TableRow, {"hbguard", "dQ2Lk8pXmN4v"}});
  sketch.parts.push_back({Kind::TableRow,
                          {"_simpleauth_sess", "\"eyJpZCI6IjY4NDE1OTI3Nz…|1791245011|a3f9c2\""},
                          true});
  sketch.parts.push_back({Kind::TableRow, {"cookie_consent", "dismissed"}});
  sketch.caption =
      firefox ? "Firefox: Storage, Cookies. Copy the value of <b>_simpleauth_sess</b>."
              : "Chrome and Edge: Application, Cookies. Copy the value of <b>_simpleauth_sess</b>.";
  return sketch;
}

SignInGuide Launcher(const QString& name, const QString& games) {
  SignInGuide guide;
  guide.title = name;
  guide.line = games + " run inside " + name + ". You sign in there, like on Windows.";
  guide.intro = games +
                " run inside their own launcher. Mira installs it, and you sign in inside it, like "
                "on Windows.";
  guide.steps = {"Install " + name + ". It runs in the background.",
                 "Open " + name + " and sign in there.",
                 "Install games in " + name + ". Mira adds them to your library."};
  guide.sketch.title = name;
  guide.sketch.parts = {Part(Kind::Heading, "Log in"), Part(Kind::Field, "Email or phone"),
                        Part(Kind::Field, "Password"), Part(Kind::Button, "Log in", true)};
  guide.sketch.caption = "After installing: you sign in here, inside " + name + ".";
  guide.done = "Installed";
  return guide;
}

}  // namespace

SignInGuide GuideFor(const std::string& id) {
  SignInGuide guide;
  guide.done = "Signed in";
  if (id == "epic") {
    guide.title = "Sign in to Epic Games";
    guide.line = "Lists the games you own on Epic and installs them.";
    guide.open = "Open the Epic sign-in page";
    guide.steps = {"Sign in with your Epic account.",
                   "The page then shows a short block of text. Press <code>Ctrl+A</code>, then "
                   "<code>Ctrl+C</code> to copy all of it. Mira picks out the code."};
    guide.sketch.address =
        "www.epicgames.com/id/api/redirect?clientId=34a02cf8f4414e29&responseType=code";
    guide.sketch.parts = {
        Part(Kind::Code,
             R"({"warning":"Do not share this code with any 3rd party service.","redirectUrl":)"
             R"("https://localhost/launcher/authorized?code=7c1e05d8a3f94b6e",)"),
        Part(Kind::Code, R"("authorizationCode":"7c1e05d8a3f94b6e9d2a41c0b8f3e5a7")", true),
        Part(Kind::Code, R"("exchangeCode":null,"sid":null})")};
    guide.sketch.caption = "Copy the whole page. Mira finds <b>authorizationCode</b> in it.";
  } else if (id == "gog") {
    guide.title = "Sign in to GOG";
    guide.line = "Lists the games you own on GOG and installs them.";
    guide.open = "Open the GOG sign-in page";
    guide.steps = {"Sign in with your GOG account.", "The page goes blank. That's expected.",
                   kCopyAddress};
    guide.sketch.address = "embed.gog.com/on_login_success?origin=client&code=Kx9pR2vQ8mWZ4tLbN0yA";
    guide.sketch.address_marked = true;
    guide.sketch.parts = {Part(Kind::Blank, "(blank page)")};
    guide.sketch.caption = "Copy the address. The page itself stays empty.";
  } else if (id == "amazon") {
    guide.title = "Sign in to Amazon Games";
    guide.line = "Lists the games you own on Amazon, including Prime Gaming, and installs them.";
    guide.open = "Open the Amazon sign-in page";
    guide.steps = {"Sign in with your Amazon account.",
                   "Amazon shows an error or a blank page. That's expected.", kCopyAddress};
    guide.sketch.address =
        "www.amazon.com/"
        "?openid.assoc_handle=amzn_sonic_games_launcher&openid.oa2.authorization_code=ANbXqzKpLmVt";
    guide.sketch.address_marked = true;
    guide.sketch.parts = {Part(Kind::Heading, "Sorry, something went wrong"),
                          Part(Kind::Text, "The page you asked for isn't available.")};
    guide.sketch.caption = "The error is expected. Copy the address.";
  } else if (id == "itch") {
    guide.title = "Connect itch.io";
    guide.line = "Lists the games you own on itch.io and installs them.";
    guide.open = "Open itch.io API keys";
    guide.intro = "itch.io uses a key instead of a sign-in.";
    guide.steps = {"Sign in if itch.io asks.", "Press <b>Generate new API key</b>.",
                   "Copy the key that appears. Press View if it's hidden."};
    guide.sketch.address = "itch.io/user/settings/api-keys";
    guide.sketch.parts = {Part(Kind::Heading, "API keys"),
                          Part(Kind::Text, "Keys let other programs use your account."),
                          Part(Kind::Button, "Generate new API key", true, 1),
                          Part(Kind::Code, "a7Xp3QnR9vLm2KwT8bYza7Xp3QnR9vLm2KwT8bYz", true, 2)};
    guide.sketch.caption = "Press the button, then copy the new key.";
    guide.done = "Connected";
  } else if (id == "humble") {
    guide.title = "Sign in to Humble Bundle";
    guide.line = "Lists your bundles and downloads their games.";
    guide.open = "Open humblebundle.com";
    guide.steps = {
        "Sign in.", "Press <code>F12</code> to open the developer tools.",
        "Open <b>Storage</b>, then <b>Cookies</b>, then <b>https://www.humblebundle.com</b>.",
        "Find <code>_simpleauth_sess</code>. Double-click its value and copy it."};
    guide.sketch = Cookies(true);
    guide.other_steps = {
        "Sign in.", "Press <code>F12</code> to open the developer tools.",
        "Open <b>Application</b>, then <b>Cookies</b>, then <b>https://www.humblebundle.com</b>.",
        "Find <code>_simpleauth_sess</code>. Click it and copy its value from the box below."};
    guide.other_sketch = Cookies(false);
    guide.note = "This value works like a password. It stays on this computer.";
    guide.note_private = true;
  } else if (id == "steam") {
    guide.title = "List Steam games you own";
    guide.line =
        "Installed Steam games need nothing. This also lists the ones you haven't installed.";
    guide.open = "Open the Steam API key page";
    guide.intro =
        "Installed Steam games need nothing. To also list games you own but haven't installed, "
        "Mira needs a free key from Steam.";
    guide.steps = {"Sign in with your Steam account.",
                   "The form asks for a domain name. Type <code>localhost</code>.",
                   "Tick the box to agree, then press <b>Register</b>.", "Copy the key."};
    guide.sketch.address = "steamcommunity.com/dev/apikey";
    guide.sketch.parts = {Part(Kind::Heading, "Register Steam Web API Key"),
                          Part(Kind::Text, "Domain Name:"),
                          Part(Kind::Field, "localhost", true, 1),
                          Part(Kind::Button, "Register"),
                          Part(Kind::Text, "Your Steam Web API Key"),
                          Part(Kind::Code, "8C1F04D2E7A93B5F6D20C8E1A47B9F35", true, 2)};
    guide.sketch.caption = "Type localhost, register, then copy the key.";
    guide.note = "Steam only gives keys to accounts that have spent at least US$5.";
    guide.done = "Key saved";
  } else if (id == "battlenet") {
    guide = Launcher("Battle.net", "Blizzard and Activision games");
  } else if (id == "ea") {
    guide = Launcher("EA app", "EA games");
  } else if (id == "ubisoft") {
    guide = Launcher("Ubisoft Connect", "Ubisoft games");
  } else if (id == "office") {
    guide.title = "Microsoft 365";
    guide.line =
        "Word, Excel, PowerPoint, Outlook, OneNote, Access and Publisher, from your Microsoft 365 "
        "subscription.";
    guide.note =
        "You sign in inside Word or Excel the first time you open one, like on Windows. Mira "
        "never sees your account.";
    guide.note_private = true;
    guide.done = "Installed";
  }
  return guide;
}

}  // namespace mira_gui
