// The profiles file (profiles.hpp): what is written is what is read, what a
// file may not hold (a password above all), that a failed save leaves the old
// file whole, and what is taken from legacy pychron's preferences. Every
// name, login and id here is made up, and no address is the service's.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/profiles.hpp"

// A namespace of its own: the suite is called Profiles, as the type is.
namespace pychron::earthbank::profiles_test {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTestUrl = "https://earthbank.test";

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& file, std::string_view bytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

long process_id() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool any_note_has(const earthbank::Profiles& profiles, std::string_view part) {
  return std::any_of(profiles.notes.begin(), profiles.notes.end(), [&](const std::string& n) { return has(n, part); });
}

// An environment variable set, or unset, until the end of the scope.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, const std::optional<std::string>& value)
      : name_(std::move(name)), before_(env_var(name_.c_str())) {
    put(value);
  }
  ~ScopedEnv() { put(before_); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  void put(const std::optional<std::string>& value) const {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
    if (value) setenv(name_.c_str(), value->c_str(), 1);
    else unsetenv(name_.c_str());
#endif
  }
  std::string name_;
  std::optional<std::string> before_;
};

Profile profile(std::string name, std::string username, std::optional<std::int64_t> institution = {},
                std::optional<std::int64_t> package = {}) {
  Profile p;
  p.name = std::move(name);
  p.base_url = std::string(kTestUrl);
  p.username = std::move(username);
  p.institution_id = institution;
  p.data_package_id = package;
  return p;
}

class Profiles : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    // Per process as well as per test: two build trees may run at once.
    dir_ = fs::temp_directory_path() /
           (std::string("pychron_profiles_") + info->name() + "_" + std::to_string(process_id()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir_, ec);
  }

  fs::path file() const { return dir_ / "profiles.toml"; }

  // `text` as the profiles file, read.
  Result<earthbank::Profiles> read(std::string_view text) const {
    spit(file(), text);
    return load_profiles(file());
  }

  // That `text` is refused as configuration, the message naming the file and
  // each of `words`.
  void expect_refused(std::string_view text, std::initializer_list<std::string_view> words) const {
    const auto loaded = read(text);
    ASSERT_FALSE(loaded.has_value()) << text;
    EXPECT_EQ(loaded.error().kind, ErrorKind::Config) << text;
    EXPECT_TRUE(has(loaded.error().what, file().string())) << loaded.error().what;
    for (const auto word : words) EXPECT_TRUE(has(loaded.error().what, word)) << word << " in: " << loaded.error().what;
  }

  // That saving `profiles` is refused as configuration and writes nothing.
  void expect_not_saved(const earthbank::Profiles& profiles, std::string_view word) const {
    const std::vector<std::string> names = names_in_dir();
    const std::string before = slurp(file());
    const auto saved = save_profiles(profiles, file());
    ASSERT_FALSE(saved.has_value()) << word;
    EXPECT_EQ(saved.error().kind, ErrorKind::Config) << saved.error().what;
    EXPECT_TRUE(has(saved.error().what, word)) << word << " in: " << saved.error().what;
    EXPECT_EQ(names_in_dir(), names) << word;
    EXPECT_EQ(slurp(file()), before) << word;
  }

  std::vector<std::string> names_in_dir() const {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir_)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
  }

  fs::path dir_;
};

TEST_F(Profiles, RoundTrip) {
  earthbank::Profiles written;
  written.active = "mylab";
  // Not in the order of their names: the file keeps the order given.
  written.profiles = {profile("second-lab", "s.sample"), profile("mylab", "r.fixture", 1001, 4321),
                      profile("Lab_3", R"(odd "name" \ here)", 7)};
  written.profiles[2].base_url = "http://localhost:8080";
  const auto saved = save_profiles(written, file());
  ASSERT_TRUE(saved.has_value()) << to_string(saved.error());

  const auto loaded = load_profiles(file());
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  EXPECT_EQ(loaded->active, "mylab");
  EXPECT_EQ(loaded->profiles, written.profiles);
  EXPECT_TRUE(loaded->notes.empty());
  ASSERT_NE(loaded->active_profile(), nullptr);
  EXPECT_EQ(loaded->active_profile()->username, "r.fixture");
  ASSERT_NE(loaded->find("second-lab"), nullptr);
  EXPECT_EQ(loaded->find("second-lab")->institution_id, std::nullopt);
  EXPECT_EQ(loaded->find("nobody"), nullptr);
  // Only the file is left: nothing of the temporary one.
  EXPECT_EQ(names_in_dir(), std::vector<std::string>{"profiles.toml"});

  // Saved again it is the same file, byte for byte.
  const std::string first = slurp(file());
  ASSERT_TRUE(save_profiles(*loaded, file()).has_value());
  EXPECT_EQ(slurp(file()), first);
}

TEST_F(Profiles, TheFileIsAsTheGuideShowsIt) {
  const auto loaded = read("active = \"mylab\"\n"
                           "\n"
                           "[profiles.mylab]\n"
                           "base_url = \"https://earthbank.test/\"\n"
                           "username = \"a.user\"\n"
                           "institution_id = 1001          # your institution's id on EarthBank\n"
                           "data_package_id = 12345\n"
                           "\n"
                           "[profiles.other]\n"
                           "username = \"b.user\"\n");
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  ASSERT_EQ(loaded->profiles.size(), 2u);
  const Profile& mine = loaded->profiles[0];
  EXPECT_EQ(mine.name, "mylab");
  // The slash at the end is dropped.
  EXPECT_EQ(mine.base_url, "https://earthbank.test");
  EXPECT_EQ(mine.username, "a.user");
  EXPECT_EQ(mine.institution_id, 1001);
  EXPECT_EQ(mine.data_package_id, 12345);
  // A profile that names no server is on the service.
  EXPECT_EQ(loaded->profiles[1].base_url, kDefaultBaseUrl);
  EXPECT_EQ(loaded->profiles[1].institution_id, std::nullopt);

  // And what is written is that: a table a profile, no key for what is unset.
  earthbank::Profiles one;
  one.active = "mylab";
  one.profiles = {profile("mylab", "a.user", 1001, 12345), profile("other", "")};
  ASSERT_TRUE(save_profiles(one, file()).has_value());
  EXPECT_EQ(slurp(file()), "active = \"mylab\"\n"
                           "\n"
                           "[profiles.mylab]\n"
                           "base_url = \"https://earthbank.test\"\n"
                           "username = \"a.user\"\n"
                           "institution_id = 1001\n"
                           "data_package_id = 12345\n"
                           "\n"
                           "[profiles.other]\n"
                           "base_url = \"https://earthbank.test\"\n"
                           "username = \"\"\n");
}

TEST_F(Profiles, AMissingFileIsEmpty) {
  const auto loaded = load_profiles(dir_ / "nowhere" / "profiles.toml");
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  EXPECT_TRUE(loaded->active.empty());
  EXPECT_TRUE(loaded->profiles.empty());
  EXPECT_EQ(loaded->active_profile(), nullptr);

  // So is an empty one. No path at all is not a file that is missing.
  const auto empty = read("");
  ASSERT_TRUE(empty.has_value()) << to_string(empty.error());
  EXPECT_TRUE(empty->profiles.empty());
  const auto none = load_profiles({});
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
  EXPECT_FALSE(save_profiles({}, {}).has_value());
}

// A file that is there and cannot be read is not a file that is missing: read
// as no profiles, the next save would put nothing where the user's were.
TEST_F(Profiles, AFileThatCannotBeReadIsAnErrorNotEmpty) {
  const fs::path folder = dir_ / "a-directory";
  fs::create_directories(folder);
  const auto of_folder = load_profiles(folder);
  ASSERT_FALSE(of_folder.has_value());
  EXPECT_EQ(of_folder.error().kind, ErrorKind::Io) << of_folder.error().what;
  EXPECT_TRUE(has(of_folder.error().what, folder.string())) << of_folder.error().what;

  const std::string text = "[profiles.mylab]\nbase_url = \"https://earthbank.test\"\nusername = \"r.fixture\"\n";
  spit(file(), text);
  std::error_code ec;
  fs::permissions(file(), fs::perms::none, fs::perm_options::replace, ec);
  {
    std::ifstream probe(file(), std::ios::binary);
    if (probe.is_open()) {
      probe.close();
      fs::permissions(file(), fs::perms::owner_all, fs::perm_options::add, ec);
      GTEST_SKIP() << "the file cannot be made unreadable here (running as root, or on a file system without "
                      "permissions), so a failing read cannot be staged";
    }
  }
  const auto closed = load_profiles(file());
  fs::permissions(file(), fs::perms::owner_all, fs::perm_options::add, ec);
  ASSERT_FALSE(closed.has_value());
  EXPECT_EQ(closed.error().kind, ErrorKind::Io) << closed.error().what;
  EXPECT_TRUE(has(closed.error().what, file().string())) << closed.error().what;
  EXPECT_EQ(slurp(file()), text);
}

// A link to a file that is not there (a dotfiles directory not mounted) is
// not "no profiles" either: read as none, the next save would replace the
// user's link with a file of its own.
TEST_F(Profiles, ALinkToNothingIsAnErrorNotEmpty) {
  const fs::path target = dir_ / "not-mounted" / "profiles.toml";
  std::error_code ec;
  fs::create_symlink(target, file(), ec);
  if (ec) GTEST_SKIP() << "no symbolic link can be made here: " << ec.message();
  const auto loaded = load_profiles(file());
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().kind, ErrorKind::Io) << loaded.error().what;
  EXPECT_TRUE(has(loaded.error().what, file().string())) << loaded.error().what;
  EXPECT_TRUE(has(loaded.error().what, "link")) << loaded.error().what;

  const auto saved = save_profiles(earthbank::Profiles{}, file());
  ASSERT_FALSE(saved.has_value());
  EXPECT_EQ(saved.error().kind, ErrorKind::Io) << saved.error().what;
  EXPECT_TRUE(fs::is_symlink(file())) << "the user's link was replaced";
  EXPECT_EQ(fs::read_symlink(file()), target);
}

TEST_F(Profiles, SaveMakesTheDirectory) {
  const fs::path deep = dir_ / "pychron" / "earthbank" / "profiles.toml";
  earthbank::Profiles one;
  one.profiles = {profile("mylab", "r.fixture")};
  const auto saved = save_profiles(one, deep);
  ASSERT_TRUE(saved.has_value()) << to_string(saved.error());
  const auto loaded = load_profiles(deep);
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  EXPECT_EQ(loaded->profiles, one.profiles);
  // No profile was chosen, and none is the active one.
  EXPECT_TRUE(loaded->active.empty());
  EXPECT_EQ(loaded->active_profile(), nullptr);
}

TEST_F(Profiles, NoPasswordKeyIsEverWrittenOrAccepted) {
  const std::string made_up = "hunter2-not-a-real-one";
  for (const std::string key :
       {"password", "Password", "PASSWORD", "token", "Token", "secret", "SECRET", "api_token"}) {
    const std::string line = std::string(key).append(" = \"").append(made_up).append("\"\n");
    // In a profile, and at the top of the file.
    for (const std::string& text : {"[profiles.mylab]\nusername = \"r.fixture\"\n" + line,
                                    line + "[profiles.mylab]\nusername = \"r.fixture\"\n"}) {
      const auto loaded = read(text);
      ASSERT_FALSE(loaded.has_value()) << key;
      EXPECT_EQ(loaded.error().kind, ErrorKind::Config) << key;
      EXPECT_TRUE(has(loaded.error().what, key)) << loaded.error().what;
      EXPECT_TRUE(has(loaded.error().what, file().string())) << loaded.error().what;
      // It says why, and does not repeat what the key held.
      EXPECT_TRUE(has(loaded.error().what, "never kept in this file")) << loaded.error().what;
      EXPECT_FALSE(has(loaded.error().what, made_up)) << loaded.error().what;
    }
  }

  // Nothing that is written has such a key, whatever a profile holds.
  earthbank::Profiles all;
  all.active = "mylab";
  all.profiles = {profile("mylab", "r.fixture", 1001, 4321), profile("other", "s.sample")};
  ASSERT_TRUE(save_profiles(all, file()).has_value());
  const std::string text = lowered(slurp(file()));
  for (const std::string_view word : {"password", "token", "secret"}) EXPECT_FALSE(has(text, word)) << word;
}

TEST_F(Profiles, UnknownKeyIsRefused) {
  expect_refused("[profiles.mylab]\nusername = \"r.fixture\"\nuser = \"r.fixture\"\n", {"profiles.mylab.user"});
  expect_refused("activ = \"mylab\"\n[profiles.mylab]\nusername = \"r.fixture\"\n", {"activ"});
  expect_refused("[profile.mylab]\nusername = \"r.fixture\"\n", {"profile"});
  // A value of the wrong kind is named by its key as well.
  expect_refused("[profiles.mylab]\nusername = 7\n", {"profiles.mylab.username"});
  expect_refused("[profiles.mylab]\ninstitution_id = \"1001\"\n", {"profiles.mylab.institution_id"});
  expect_refused("[profiles.mylab]\ndata_package_id = 0\n", {"profiles.mylab.data_package_id"});
  expect_refused("[profiles.mylab]\ndata_package_id = 1.5\n", {"profiles.mylab.data_package_id"});
  expect_refused("[profiles]\nmylab = \"r.fixture\"\n", {"profiles.mylab"});
  expect_refused("profiles = 3\n", {"profiles"});
  expect_refused("active = 3\n", {"active"});
  // And a file that is not TOML at all.
  expect_refused("[profiles.mylab\nusername = \n", {});
}

TEST_F(Profiles, NamesAreLettersDigitsDashAndUnderscore) {
  for (const std::string_view good : {"mylab", "My-Lab_2", "a", "0"}) EXPECT_TRUE(valid_profile_name(good)) << good;
  // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): one name; joined, the escape would take the `b` as a hex digit
  for (const std::string_view bad : {"", "my lab", "lab.one", "lab@host", "lab/one", "l\xC3\xA4" "b", "lab:1"})
    EXPECT_FALSE(valid_profile_name(bad)) << bad;

  expect_refused("[profiles.\"my lab\"]\nusername = \"r.fixture\"\n", {"my lab"});
  expect_refused("[profiles.\"lab.one\"]\nusername = \"r.fixture\"\n", {"lab.one"});
  // Two that the Credential Manager would keep one password for.
  expect_refused("[profiles.MyLab]\nusername = \"r.fixture\"\n[profiles.mylab]\nusername = \"s.sample\"\n",
                 {"MyLab", "mylab"});

  earthbank::Profiles bad;
  bad.profiles = {profile("my lab", "r.fixture")};
  expect_not_saved(bad, "my lab");
  bad.profiles = {profile("mylab", "r.fixture"), profile("mylab", "s.sample")};
  expect_not_saved(bad, "mylab");
  bad.profiles = {profile("mylab", "r.fixture"), profile("MYLAB", "s.sample")};
  expect_not_saved(bad, "MYLAB");
  bad.profiles = {profile("mylab", "r.fixture", 0)};
  expect_not_saved(bad, "institution_id");
}

TEST_F(Profiles, BaseUrlMustBeHttps) {
  for (const std::string_view good :
       {"https://earthbank.test", "https://earthbank.test:8443", "https://EarthBank.Test", "https://10.0.0.7",
        "http://localhost", "http://localhost:8080", "http://127.0.0.1:5000", "http://[::1]:5000", "https://[::1]"}) {
    const auto url = checked_base_url(good);
    ASSERT_TRUE(url.has_value()) << good << ": " << to_string(url.error());
    EXPECT_EQ(*url, good);
  }
  // One slash at the end is dropped, and only one.
  EXPECT_EQ(checked_base_url("https://earthbank.test/").value_or("?"), "https://earthbank.test");
  EXPECT_EQ(checked_base_url("http://localhost:8080/").value_or("?"), "http://localhost:8080");

  for (const std::string_view bad :
       {"", "earthbank.test", "http://earthbank.test", "http://localhost.earthbank.test", "http://127.0.0.2",
        "ftp://earthbank.test", "HTTPS://earthbank.test", "https://", "https:///api", "https://earthbank.test/api",
        "https://earthbank.test//", "https://earthbank.test?x=1", "https://earthbank.test/?x=1",
        "https://earthbank.test#top", "https://r.fixture@earthbank.test", "https://r.fixture:pw@earthbank.test",
        "https://earthbank.test:", "https://earthbank.test:port", "https://earthbank.test:99999",
        "https://earth bank.test", " https://earthbank.test", "https://earthbank.test\n", "https://[::1",
        "file:///etc"}) {
    const auto url = checked_base_url(bad);
    ASSERT_FALSE(url.has_value()) << bad;
    EXPECT_EQ(url.error().kind, ErrorKind::Config) << bad;
  }
  // What is wrong with it is said without a password that was in it.
  EXPECT_FALSE(has(checked_base_url("https://r.fixture:made-up-pw@earthbank.test").error().what, "made-up-pw"));

  expect_refused("[profiles.mylab]\nbase_url = \"http://earthbank.test\"\n", {"profiles.mylab.base_url", "https://"});
  expect_refused("[profiles.mylab]\nbase_url = \"https://earthbank.test/api\"\n", {"profiles.mylab.base_url"});
  expect_refused("[profiles.mylab]\nbase_url = 3\n", {"profiles.mylab.base_url"});

  earthbank::Profiles bad;
  bad.profiles = {profile("mylab", "r.fixture")};
  bad.profiles[0].base_url = "http://earthbank.test";
  expect_not_saved(bad, "base_url");
}

TEST_F(Profiles, ActiveMustNameAProfile) {
  expect_refused("active = \"other\"\n[profiles.mylab]\nusername = \"r.fixture\"\n", {"active", "other"});
  expect_refused("active = \"mylab\"\n", {"active", "mylab"});
  // Names are compared exactly.
  expect_refused("active = \"MyLab\"\n[profiles.mylab]\nusername = \"r.fixture\"\n", {"active", "MyLab"});

  earthbank::Profiles bad;
  bad.active = "other";
  bad.profiles = {profile("mylab", "r.fixture")};
  expect_not_saved(bad, "other");

  // None chosen is allowed: there is then no active profile.
  const auto loaded = read("[profiles.mylab]\nusername = \"r.fixture\"\n");
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  EXPECT_EQ(loaded->active_profile(), nullptr);
}

TEST_F(Profiles, SaveIsAtomic) {
  earthbank::Profiles first;
  first.active = "mylab";
  first.profiles = {profile("mylab", "r.fixture", 1001, 4321)};
  ASSERT_TRUE(save_profiles(first, file()).has_value());
  const std::string before = slurp(file());
  ASSERT_FALSE(before.empty());

  // No file can be made beside it now: the temporary one cannot be written.
  std::error_code ec;
  fs::permissions(dir_, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, ec);
  {
    std::ofstream probe(dir_ / "probe");
    if (probe.is_open()) {
      probe.close();
      fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::add, ec);
      fs::remove(dir_ / "probe", ec);
      GTEST_SKIP() << "the directory cannot be made unwritable here (running as root, or on a file system "
                      "without permissions), so a failing write cannot be staged";
    }
  }

  earthbank::Profiles second = first;
  second.profiles.push_back(profile("other", "s.sample"));
  second.active = "other";
  const auto saved = save_profiles(second, file());
  ASSERT_FALSE(saved.has_value());
  EXPECT_EQ(saved.error().kind, ErrorKind::Io);
  EXPECT_TRUE(has(saved.error().what, file().string())) << saved.error().what;

  EXPECT_EQ(slurp(file()), before);
  const auto loaded = load_profiles(file());
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  EXPECT_EQ(loaded->profiles, first.profiles);

  fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::add, ec);
  EXPECT_EQ(names_in_dir(), std::vector<std::string>{"profiles.toml"});
  // And with the directory writable again the same save goes through.
  ASSERT_TRUE(save_profiles(second, file()).has_value());
  EXPECT_EQ(load_profiles(file()).value_or(earthbank::Profiles{}).profiles, second.profiles);
}

#ifndef _WIN32
TEST_F(Profiles, TheFileIsItsOwnersOnly) {
  earthbank::Profiles one;
  one.profiles = {profile("mylab", "r.fixture")};
  // Whatever was there before, and however open it was.
  spit(file(), "");
  fs::permissions(file(), fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
  ASSERT_TRUE(save_profiles(one, file()).has_value());
  EXPECT_EQ(fs::status(file()).permissions(), fs::perms::owner_read | fs::perms::owner_write);
}
#endif

TEST_F(Profiles, DefaultFileIsUnderTheUsersConfigDirectory) {
  // The directory is the one libs/setup keeps the site file in: `Pychron` on
  // macOS and Windows, `pychron` on Linux.
#if defined(_WIN32) || defined(__APPLE__)
  const fs::path tail = fs::path("Pychron") / "earthbank" / "profiles.toml";
#else
  const fs::path tail = fs::path("pychron") / "earthbank" / "profiles.toml";
#endif
#if defined(_WIN32)
  const ScopedEnv appdata("APPDATA", dir_.string());
  EXPECT_EQ(default_profiles_file(), dir_ / tail);
#elif defined(__APPLE__)
  const ScopedEnv home("HOME", dir_.string());
  EXPECT_EQ(default_profiles_file(), dir_ / "Library" / "Application Support" / tail);
  {
    const ScopedEnv homeless("HOME", std::nullopt);
    EXPECT_TRUE(default_profiles_file().empty());
  }
#else
  const ScopedEnv home("HOME", dir_.string());
  {
    const ScopedEnv xdg("XDG_CONFIG_HOME", (dir_ / "xdg").string());
    EXPECT_EQ(default_profiles_file(), dir_ / "xdg" / tail);
  }
  {
    // Unset, empty, or not a whole path (which the XDG rules say to ignore).
    const ScopedEnv unset("XDG_CONFIG_HOME", std::nullopt);
    EXPECT_EQ(default_profiles_file(), dir_ / ".config" / tail);
    const ScopedEnv relative("XDG_CONFIG_HOME", "config");
    EXPECT_EQ(default_profiles_file(), dir_ / ".config" / tail);
    const ScopedEnv homeless("HOME", std::nullopt);
    EXPECT_TRUE(default_profiles_file().empty());
  }
#endif
}

// The preferences file as legacy pychron wrote it: one section a plugin, the
// profiles as one line of JSON in quotes (it has commas), ids as text.
constexpr std::string_view kLegacyIni =
    "[pychron.general]\n"
    "organization = ExampleLab\n"
    "password = not-this-section's-business\n"
    "\n"
    "[pychron.ausgeochem]\n"
    "profiles_json = '[{\"name\": \"default\", \"base_url\": \"https://earthbank.test\", \"username\": \"r.fixture\", "
    "\"data_package_id\": \"4321\", \"institution_id\": \"1001\"}, "
    "{\"name\": \"second lab\", \"base_url\": \"https://earthbank.test/\", \"username\": \"s.sample\", "
    "\"data_package_id\": \"\", \"institution_id\": \"\"}, "
    "{\"name\": \"profile1\", \"username\": \"\", \"data_package_id\": 77, \"institution_id\": \"n/a\"}, "
    "{\"name\": \"old\", \"base_url\": \"http://earthbank.test\", \"username\": \"t.tester\"}]'\n"
    "active_profile = second lab\n"
    "\n"
    "[pychron.dvc]\n"
    "active_profile = somebody-else\n";

TEST_F(Profiles, ImportLegacyIni) {
  const fs::path ini = dir_ / "pychron.ausgeochem.ini";
  spit(ini, kLegacyIni);
  const auto imported = import_legacy_ini(ini);
  ASSERT_TRUE(imported.has_value()) << to_string(imported.error());
  ASSERT_EQ(imported->profiles.size(), 3u);

  EXPECT_EQ(imported->profiles[0], profile("default", "r.fixture", 1001, 4321));
  // A name with a space in it is respelled, and it is still the active one.
  EXPECT_EQ(imported->profiles[1], profile("second_lab", "s.sample"));
  EXPECT_EQ(imported->active, "second_lab");
  EXPECT_TRUE(any_note_has(*imported, "second lab")) << ::testing::PrintToString(imported->notes);
  // No address: the service's. A number as a number; what is not one is left out.
  const Profile& third = imported->profiles[2];
  EXPECT_EQ(third.name, "profile1");
  EXPECT_EQ(third.base_url, kDefaultBaseUrl);
  EXPECT_EQ(third.data_package_id, 77);
  EXPECT_EQ(third.institution_id, std::nullopt);
  EXPECT_TRUE(any_note_has(*imported, "n/a")) << ::testing::PrintToString(imported->notes);
  // An address that would be refused: the profile is left out, and said to be.
  EXPECT_EQ(imported->find("old"), nullptr);
  EXPECT_TRUE(any_note_has(*imported, "old")) << ::testing::PrintToString(imported->notes);

  // What comes out can be saved and read as it is.
  ASSERT_TRUE(save_profiles(*imported, file()).has_value());
  const auto loaded = load_profiles(file());
  ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
  EXPECT_EQ(loaded->profiles, imported->profiles);
  EXPECT_EQ(loaded->active, "second_lab");
}

TEST_F(Profiles, ImportIgnoresAPasswordAndSaysSo) {
  const std::string made_up = "hunter2-not-a-real-one";
  const fs::path ini = dir_ / "pychron.ausgeochem.ini";
  // In a profile, and as a key of the section: neither is taken.
  spit(ini, "[pychron.ausgeochem]\n"
            "profiles_json = '[{\"name\": \"default\", \"base_url\": \"https://earthbank.test\", "
            "\"username\": \"r.fixture\", \"password\": \"" + made_up + "\"}, "
            "{\"name\": \"other\", \"base_url\": \"https://earthbank.test\", \"username\": \"s.sample\"}]'\n"
            "active_profile = default\n"
            "Password = " + made_up + "\n");
  const auto imported = import_legacy_ini(ini);
  ASSERT_TRUE(imported.has_value()) << to_string(imported.error());
  ASSERT_EQ(imported->profiles.size(), 2u);
  EXPECT_EQ(imported->profiles[0], profile("default", "r.fixture"));

  // One note for the profile, one for the section; neither repeats it.
  std::size_t said = 0;
  for (const std::string& note : imported->notes) {
    EXPECT_FALSE(has(note, made_up)) << note;
    if (has(lowered(note), "password") && has(note, "ignored")) ++said;
  }
  EXPECT_EQ(said, 2u) << ::testing::PrintToString(imported->notes);
  EXPECT_TRUE(any_note_has(*imported, "default")) << ::testing::PrintToString(imported->notes);

  ASSERT_TRUE(save_profiles(*imported, file()).has_value());
  EXPECT_FALSE(has(slurp(file()), made_up));

  // A file with no password in it has no such note.
  spit(ini, kLegacyIni);
  const auto plain = import_legacy_ini(ini);
  ASSERT_TRUE(plain.has_value()) << to_string(plain.error());
  for (const std::string& note : plain->notes) EXPECT_FALSE(has(lowered(note), "password")) << note;
}

TEST_F(Profiles, ImportReadsTheQuotingLegacyWrote) {
  const fs::path ini = dir_ / "preferences.ini";
  const std::string entry = "[{\"name\": \"default\", \"base_url\": \"https://earthbank.test\", "
                            "\"username\": \"o'fixture\"}]";
  // A value with both kinds of quote in it was written in three of one kind;
  // one with none needing it, bare. Windows line ends, and keys with no
  // section above them (a file of this plugin's keys alone).
  for (const std::string& text :
       {"[pychron.ausgeochem]\r\nprofiles_json = \"\"\"" + entry + "\"\"\"\r\nactive_profile = \"default\"\r\n",
        "[pychron.ausgeochem]\nprofiles_json = '''" + entry + "'''\nactive_profile = 'default'   # chosen\n",
        "# legacy\nprofiles_json = '''" + entry + "'''\nactive_profile = default\n"}) {
    spit(ini, text);
    const auto imported = import_legacy_ini(ini);
    ASSERT_TRUE(imported.has_value()) << text << ": " << to_string(imported.error());
    ASSERT_EQ(imported->profiles.size(), 1u) << text;
    EXPECT_EQ(imported->profiles[0], profile("default", "o'fixture")) << text;
    EXPECT_EQ(imported->active, "default") << text;
  }

  // An active profile that is not among them: the first, as legacy did.
  spit(ini, "[pychron.ausgeochem]\nprofiles_json = '" + std::string("[{\"name\": \"a\", \"base_url\": "
            "\"https://earthbank.test\"}, {\"name\": \"b\", \"base_url\": \"https://earthbank.test\"}]") +
            "'\nactive_profile = gone\n");
  const auto first = import_legacy_ini(ini);
  ASSERT_TRUE(first.has_value()) << to_string(first.error());
  EXPECT_EQ(first->active, "a");
  EXPECT_TRUE(any_note_has(*first, "gone")) << ::testing::PrintToString(first->notes);

  // Two names that come to the same one: the second is left out.
  spit(ini, "[pychron.ausgeochem]\nprofiles_json = '" + std::string("[{\"name\": \"my lab\", \"base_url\": "
            "\"https://earthbank.test\"}, {\"name\": \"My_Lab\", \"base_url\": \"https://earthbank.test\"}]") + "'\n");
  const auto twice = import_legacy_ini(ini);
  ASSERT_TRUE(twice.has_value()) << to_string(twice.error());
  ASSERT_EQ(twice->profiles.size(), 1u);
  EXPECT_EQ(twice->profiles[0].name, "my_lab");
  EXPECT_TRUE(any_note_has(*twice, "My_Lab")) << ::testing::PrintToString(twice->notes);
}

TEST_F(Profiles, ImportRefusesWhatIsNotLegacysFile) {
  const fs::path ini = dir_ / "preferences.ini";
  const auto missing = import_legacy_ini(ini);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().kind, ErrorKind::Io);
  EXPECT_TRUE(has(missing.error().what, ini.string())) << missing.error().what;

  for (const std::string_view text :
       {"[pychron.dvc]\norganization = ExampleLab\n", "[pychron.ausgeochem]\nprofiles_json = '[{\"name\": '\n",
        "[pychron.ausgeochem]\nprofiles_json = '{\"name\": \"default\"}'\n"}) {
    spit(ini, text);
    const auto imported = import_legacy_ini(ini);
    ASSERT_FALSE(imported.has_value()) << text;
    EXPECT_EQ(imported.error().kind, ErrorKind::Config) << text;
    EXPECT_TRUE(has(imported.error().what, ini.string())) << imported.error().what;
  }

  // The section with nothing in it yet: no profiles, and a note that says so.
  spit(ini, "[pychron.ausgeochem]\nprofiles_json = \nactive_profile = \n");
  const auto empty = import_legacy_ini(ini);
  ASSERT_TRUE(empty.has_value()) << to_string(empty.error());
  EXPECT_TRUE(empty->profiles.empty());
  EXPECT_TRUE(empty->active.empty());
  EXPECT_FALSE(empty->notes.empty());
}

// The service is named once, as the default in profiles.hpp. Anywhere else a
// test or an example would be one edit from talking to it. Documentation
// (Markdown) may name it.
TEST_F(Profiles, TheServiceIsNamedInOneFileOnly) {
  const fs::path root = PYCHRON_EARTHBANK_REPO_DIR;
  const fs::path home = root / "libs" / "earthbank" / "include" / "pychron" / "earthbank" / "profiles.hpp";
  ASSERT_TRUE(fs::exists(home)) << home;
  // Not spelled out here either.
  const std::string host(kDefaultBaseUrl.substr(kDefaultBaseUrl.find("://") + 3));
  ASSERT_GT(host.size(), 8u);
  EXPECT_TRUE(has(slurp(home), host));

  std::vector<std::string> found;
  std::size_t files_read = 0;
  for (const char* top : {"libs", "apps", "tests"}) {
    for (const auto& entry : fs::recursive_directory_iterator(root / top)) {
      if (!entry.is_regular_file() || entry.path().extension() == ".md") continue;
      if (fs::equivalent(entry.path(), home)) continue;
      ++files_read;
      if (has(slurp(entry.path()), host)) found.push_back(fs::relative(entry.path(), root).generic_string());
    }
  }
  EXPECT_GT(files_read, 100u);
  EXPECT_TRUE(found.empty()) << ::testing::PrintToString(found);
}

}  // namespace
}  // namespace pychron::earthbank::profiles_test
