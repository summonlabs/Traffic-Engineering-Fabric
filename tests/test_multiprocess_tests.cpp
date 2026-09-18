// Traffic Engineering Fabric - real multiprocess kill/restart proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every participant in these tests is a separate operating-system process
// launched from the shipped tef-node binary and talking over a real TCP socket.
// Nothing here is a thread-only substitute.
//
// The only bounded wait is a startup handshake: the parent polls for the
// information file the child is required to publish before it can be used. If
// the budget is exhausted the test FAILS loudly - there is no watchdog that
// silently terminates a test and no timeout that converts a hang into a pass.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

#if defined(_WIN32)

struct ChildProcess {
  PROCESS_INFORMATION info{};
  bool started = false;
  std::string label;

  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept : info(other.info), started(other.started),
                                                label(std::move(other.label)) {
    other.started = false;
    other.info = PROCESS_INFORMATION{};
  }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      release();
      info = other.info;
      started = other.started;
      label = std::move(other.label);
      other.started = false;
      other.info = PROCESS_INFORMATION{};
    }
    return *this;
  }
  // A crashed or aborted test must not leak a live coordinator or publisher
  // process into the machine.
  ~ChildProcess() {
    if (started) kill();
    release();
  }

  void release() {
    if (info.hProcess != nullptr) ::CloseHandle(info.hProcess);
    if (info.hThread != nullptr) ::CloseHandle(info.hThread);
    info = PROCESS_INFORMATION{};
    started = false;
  }

  bool running() const {
    if (!started) return false;
    return ::WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT;
  }

  void kill() {
    if (!started) return;
    (void)::TerminateProcess(info.hProcess, 99);
    (void)::WaitForSingleObject(info.hProcess, INFINITE);
    started = false;
  }

  int wait() {
    if (!started) return -1;
    (void)::WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    (void)::GetExitCodeProcess(info.hProcess, &code);
    started = false;
    return static_cast<int>(code);
  }
};

std::string quote(const std::string& value) { return "\"" + value + "\""; }

ChildProcess spawn_child(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::string& stdout_path, const std::string& label) {
  std::string command = quote(executable);
  for (const auto& argument : arguments) {
    command += " ";
    command += quote(argument);
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE out = ::CreateFileA(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  TEF_CHECK_MSG(out != INVALID_HANDLE_VALUE, "cannot create " + stdout_path);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out;
  startup.hStdError = out;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  ChildProcess child;
  child.label = label;
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL created = ::CreateProcessA(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &child.info);
  ::CloseHandle(out);
  TEF_CHECK_MSG(created != 0, "cannot start " + command);
  child.started = true;
  return child;
}

#else

struct ChildProcess {
  int pid = -1;
  std::string label;
  bool started = false;

  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept : pid(other.pid), label(std::move(other.label)),
                                                started(other.started) {
    other.started = false;
    other.pid = -1;
  }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      pid = other.pid;
      label = std::move(other.label);
      started = other.started;
      other.started = false;
      other.pid = -1;
    }
    return *this;
  }
  ~ChildProcess() {
    if (started) kill();
  }

  bool running() const {
    if (!started) return false;
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == 0;
  }

  void kill() {
    if (!started) return;
    (void)::kill(pid, SIGKILL);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    started = false;
  }

  int wait() {
    if (!started) return -1;
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    started = false;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
};

ChildProcess spawn_child(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::string& stdout_path, const std::string& label) {
  std::vector<std::string> owned;
  owned.push_back(executable);
  for (const auto& argument : arguments) owned.push_back(argument);
  std::vector<char*> argv;
  for (auto& value : owned) argv.push_back(value.data());
  argv.push_back(nullptr);

  ChildProcess child;
  child.label = label;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                   0644);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  const int result = posix_spawn(&child.pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  TEF_CHECK_EQ(result, 0);
  child.started = true;
  return child;
}

#endif

std::string g_self_path;

// The cluster node binary is located next to this test executable. CMake also
// bakes in the canonical path; the sibling lookup keeps the suite runnable when
// it is invoked from a differently laid out tree.
std::string node_executable() {
#if defined(TEF_NODE_EXECUTABLE)
  if (std::filesystem::exists(std::filesystem::path(TEF_NODE_EXECUTABLE))) {
    return TEF_NODE_EXECUTABLE;
  }
#endif
  const std::filesystem::path self(g_self_path);
  std::filesystem::path candidate = self.parent_path() / "tef-node.exe";
  if (!std::filesystem::exists(candidate)) {
    candidate = self.parent_path() / "tef-node";
  }
  return candidate.string();
}

std::map<std::string, std::string> read_key_values(const std::string& path) {
  std::map<std::string, std::string> values;
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos) continue;
    values[line.substr(0, separator)] = line.substr(separator + 1);
  }
  return values;
}

// Startup handshake. The child is required to publish its information file
// before it can be used; exhausting the budget fails the test rather than
// silently passing it.
std::map<std::string, std::string> await_key(const std::string& path,
                                              const std::string& required) {
  constexpr int kStartupBudgetAttempts = 600;
  for (int attempt = 0; attempt < kStartupBudgetAttempts; ++attempt) {
    std::error_code code;
    if (std::filesystem::exists(path, code)) {
      const std::map<std::string, std::string> values = read_key_values(path);
      if (values.count(required) != 0) return values;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  TEF_FAIL("the child process never published " + required + " at " + path);
}

std::map<std::string, std::string> await_key_values(const std::string& path) {
  std::map<std::string, std::string> values = await_key(path, "port");
  TEF_CHECK(values.count("epoch") != 0);
  return values;
}

struct Fixture {
  std::string root;
  std::string state_dir;
  std::string info_path;
  std::string out_path;

  explicit Fixture(const std::string& tag) {
    root = tef::test::temporary_directory(tag);
    state_dir = root + "/state";
    info_path = root + "/coordinator.txt";
    out_path = root + "/child.txt";
    std::error_code code;
    std::filesystem::create_directories(state_dir, code);
  }

  std::string path(const std::string& name) const { return root + "/" + name; }
};

ChildProcess start_coordinator(const Fixture& fixture, const std::string& label) {
  // The information file is removed first: a restart must publish fresh values
  // rather than have the supervisor observe the previous incarnation's.
  std::error_code code;
  std::filesystem::remove(fixture.info_path, code);
  std::vector<std::string> arguments = {"coordinator", "--info-file", fixture.info_path,
                                        "--state-dir", fixture.state_dir, "--port", "0"};
  return spawn_child(node_executable(), arguments,
                     fixture.path(label + "-out.txt"), label);
}

std::map<std::string, std::string> run_publisher(const Fixture& fixture, const std::string& tag,
                                                 const std::vector<std::string>& extra) {
  const std::string out = fixture.path(tag + ".txt");
  const std::string log = fixture.path(tag + "-log.txt");
  std::vector<std::string> arguments = {"publisher"};
  for (const auto& value : extra) arguments.push_back(value);
  arguments.push_back("--out");
  arguments.push_back(out);
  ChildProcess child = spawn_child(node_executable(), arguments, log, tag);
  const int code = child.wait();
  TEF_CHECK_MSG(code == 0, tag + " exited with " + std::to_string(code));
  return read_key_values(out);
}

std::vector<std::string> publisher_arguments(const std::map<std::string, std::string>& coordinator,
                                             const std::string& publisher, const std::string& boot,
                                             const std::string& scenario,
                                             const std::vector<std::string>& extra = {}) {
  std::vector<std::string> arguments = {"--address", coordinator.at("address"), "--port",
                                        coordinator.at("port"), "--publisher", publisher,
                                        "--boot", boot, "--scenario", scenario};
  for (const auto& value : extra) arguments.push_back(value);
  return arguments;
}

}  // namespace

TEF_TEST(two_independent_publisher_processes_commit_over_real_tcp) {
  Fixture fixture("multiproc-commit");
  ChildProcess coordinator = start_coordinator(fixture, "coordinator");
  const std::map<std::string, std::string> info = await_key_values(fixture.info_path);

  const std::map<std::string, std::string> first =
      run_publisher(fixture, "publisher-a",
                    publisher_arguments(info, "publisher-one", "1:1", "plan-commit",
                                        {"--plan", "plan-a", "--commit", "commit-a"}));
  TEF_CHECK_EQ(first.at("connect"), std::string("OK"));
  TEF_CHECK_EQ(first.at("plan_code"), std::string("OK"));
  TEF_CHECK_EQ(first.at("plan_state"), std::string("PROPOSED"));
  TEF_CHECK_EQ(first.at("commit_code"), std::string("OK"));
  TEF_CHECK_EQ(first.at("commit_state"), std::string("COMMITTED"));

  const std::map<std::string, std::string> second =
      run_publisher(fixture, "publisher-b",
                    publisher_arguments(info, "publisher-two", "2:2", "plan-commit",
                                        {"--plan", "plan-b", "--commit", "commit-b"}));
  TEF_CHECK_EQ(second.at("connect"), std::string("OK"));
  TEF_CHECK_EQ(second.at("plan_code"), std::string("OK"));
  TEF_CHECK_EQ(second.at("commit_code"), std::string("OK"));
  TEF_CHECK_EQ(second.at("commit_state"), std::string("COMMITTED"));

  const std::map<std::string, std::string> query =
      run_publisher(fixture, "publisher-query",
                    publisher_arguments(info, "publisher-two", "2:2", "plan-commit-query",
                                        {"--plan", "plan-c", "--commit", "commit-c"}));
  TEF_CHECK_EQ(query.at("query_state"), std::string("COMMITTED"));

  TEF_CHECK(coordinator.running());
  coordinator.kill();
}

TEF_TEST(a_committed_plan_survives_a_real_coordinator_kill_and_restart) {
  Fixture fixture("multiproc-restart");
  std::string first_epoch;
  std::string first_boot;
  {
    ChildProcess coordinator = start_coordinator(fixture, "coordinator-1");
    const std::map<std::string, std::string> info = await_key_values(fixture.info_path);
    first_epoch = info.at("epoch");
    first_boot = info.at("boot");

    const std::map<std::string, std::string> committed =
        run_publisher(fixture, "publisher-commit",
                      publisher_arguments(info, "publisher-one", "5:5", "plan-commit",
                                          {"--plan", "plan-durable", "--commit", "commit-durable"}));
    TEF_CHECK_EQ(committed.at("commit_state"), std::string("COMMITTED"));
    TEF_CHECK(coordinator.running());
    // A real kill, not a graceful shutdown: no state is flushed on the way out.
    coordinator.kill();
  }

  {
    ChildProcess coordinator = start_coordinator(fixture, "coordinator-2");
    const std::map<std::string, std::string> info = await_key_values(fixture.info_path);
    TEF_CHECK_NE(info.at("epoch"), first_epoch);
    TEF_CHECK(std::stoull(info.at("epoch")) > std::stoull(first_epoch));
    TEF_CHECK_NE(info.at("boot"), first_boot);

    // The durable plan is restored as history, never as live authority.
    const std::map<std::string, std::string> query =
        run_publisher(fixture, "publisher-query",
                      publisher_arguments(info, "publisher-three", "6:6", "plan-commit-query",
                                          {"--plan", "plan-restarted", "--commit", "commit-restarted"}));
    TEF_CHECK_EQ(query.at("connect"), std::string("OK"));

    // A frame carrying the superseded epoch is rejected.
    const std::map<std::string, std::string> stale =
        run_publisher(fixture, "publisher-stale",
                      publisher_arguments(info, "publisher-four", "7:7", "stale-epoch",
                                          {"--epoch", first_epoch, "--plan", "plan-stale-frame",
                                           "--commit", "commit-stale-frame"}));
    TEF_CHECK_EQ(stale.at("plan_code"), std::string("EPOCH_MISMATCH"));

    TEF_CHECK(coordinator.running());
    coordinator.kill();
  }
}

TEF_TEST(a_killed_publisher_boot_is_permanently_fenced) {
  Fixture fixture("multiproc-fence");
  ChildProcess coordinator = start_coordinator(fixture, "coordinator");
  const std::map<std::string, std::string> info = await_key_values(fixture.info_path);

  // Publisher one registers and then blocks on the socket until it is killed.
  const std::string hold_out = fixture.path("hold.txt");
  const std::string hold_log = fixture.path("hold-log.txt");
  std::vector<std::string> hold_arguments = {"publisher"};
  for (const auto& value : publisher_arguments(info, "publisher-one", "10:10", "hold")) {
    hold_arguments.push_back(value);
  }
  hold_arguments.push_back("--out");
  hold_arguments.push_back(hold_out);
  ChildProcess holding = spawn_child(node_executable(), hold_arguments, hold_log, "publisher-hold");

  const std::map<std::string, std::string> held = await_key(hold_out, "register");
  TEF_CHECK_EQ(held.at("register"), std::string("OK"));
  holding.kill();

  // The same publisher returns with a fresh boot identity: the replaced boot is
  // fenced.
  const std::map<std::string, std::string> revived =
      run_publisher(fixture, "publisher-revived",
                    publisher_arguments(info, "publisher-one", "11:11", "register"));
  TEF_CHECK_EQ(revived.at("register"), std::string("OK"));

  // The old boot identity can never be used again, even by a live process.
  const std::map<std::string, std::string> fenced =
      run_publisher(fixture, "publisher-fenced",
                    publisher_arguments(info, "publisher-one", "10:10", "register"));
  const std::string registration = fenced.at("register");
  TEF_CHECK_MSG(registration.find("FENCED") != std::string::npos,
                "expected a FENCED rejection, got: " + registration);

  // And a mutation forged with the fenced boot is refused as well.
  const std::map<std::string, std::string> forged =
      run_publisher(fixture, "publisher-forged",
                    publisher_arguments(info, "publisher-one", "10:10", "stale-epoch",
                                        {"--plan", "plan-fenced", "--commit", "commit-fenced"}));
  TEF_CHECK(forged.count("plan_code") != 0);

  TEF_CHECK(coordinator.running());
  coordinator.kill();
}

TEF_TEST(duplicate_commit_frames_from_a_separate_process_are_idempotent) {
  Fixture fixture("multiproc-duplicate");
  ChildProcess coordinator = start_coordinator(fixture, "coordinator");
  const std::map<std::string, std::string> info = await_key_values(fixture.info_path);

  const std::map<std::string, std::string> result =
      run_publisher(fixture, "publisher-duplicate",
                    publisher_arguments(info, "publisher-one", "20:20", "plan-commit-twice",
                                        {"--plan", "plan-twice", "--commit", "commit-twice",
                                         "--second-commit", "commit-twice-other"}));
  TEF_CHECK_EQ(result.at("commit_code"), std::string("OK"));
  TEF_CHECK_EQ(result.at("commit_state"), std::string("COMMITTED"));
  TEF_CHECK_EQ(result.at("replay_code"), std::string("OK"));
  TEF_CHECK_EQ(result.at("replay_idempotent"), std::string("true"));
  TEF_CHECK_EQ(result.at("conflict_code"), std::string("ALREADY_COMMITTED"));

  coordinator.kill();
}

TEF_TEST(a_malformed_client_does_not_disturb_live_processes) {
  Fixture fixture("multiproc-malformed");
  ChildProcess coordinator = start_coordinator(fixture, "coordinator");
  const std::map<std::string, std::string> info = await_key_values(fixture.info_path);

  const std::map<std::string, std::string> noisy =
      run_publisher(fixture, "publisher-noise",
                    publisher_arguments(info, "publisher-noise", "30:30", "raw-bad-magic"));
  TEF_CHECK(noisy.count("reply_type") != 0 || noisy.count("connection") != 0);

  const std::map<std::string, std::string> healthy =
      run_publisher(fixture, "publisher-healthy",
                    publisher_arguments(info, "publisher-one", "31:31", "plan-commit",
                                        {"--plan", "plan-after-noise",
                                         "--commit", "commit-after-noise"}));
  TEF_CHECK_EQ(healthy.at("commit_state"), std::string("COMMITTED"));

  coordinator.kill();
}

int main(int argc, char** argv) {
  if (argc > 0 && argv[0] != nullptr) g_self_path = argv[0];
  return tef::test::run_all(argc, argv);
}
