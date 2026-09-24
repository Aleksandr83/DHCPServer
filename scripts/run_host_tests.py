"""Host test harness for DHCPServer.

The tests in test/test_*.cpp are written for ESP-IDF, but most of them need
nothing but a compiler: they run on the PC through a small shim that calls the
test's app_main (the recipe lives in the file's own header comment).  This
script builds and runs all of them, so the step is one command instead of
thirty, and it keeps the recipes honest by making a missing translation unit
visible here rather than in someone's memory.

Usage (paths are resolved against this file's own location, so the current
directory does not matter):

    <idf-venv>\\Scripts\\python.exe scripts\\run_host_tests.py
    ... scripts\\run_host_tests.py -v                 # a failing test's output
    ... scripts\\run_host_tests.py filejson cache     # only matching tests
    ... scripts\\run_host_tests.py --mingw C:\\Qt\\Tools\\mingw1310_64\\bin

For every test file it:
  * reads the sources, libraries and -I flags from the file's own
    "Build (MinGW...)" comment;
  * adds the translation units that recipe forgets -- EXTRA_UNITS below, the
    debt noted in Plan/Memory.md section 9, plus everything reachable through
    the file's quoted #include closure;
  * decides the entry point: no shim when the file defines main(), otherwise a
    shim for void/int app_main with C or C++ linkage, trying each until it links;
  * compiles with -fsyntax-only first, so "does not compile" is told apart from
    "does not link";
  * runs the executable from the repository root and writes one log per test to
    <work>\\logs.

Everything it generates (shims, executables, logs) lives in
%TEMP%\\dhcpserver_host_tests and never in the repository.

Exit code: 0 when every test either passed or is listed in KNOWN_NOT_BUILT;
1 when a test outside that list does not build, or a runnable test fails.  That
list is a baseline, not an excuse -- a compile error in a test that used to
build has to be visible in the exit code.
"""
import os
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(tempfile.gettempdir(), "dhcpserver_host_tests")
BIN = os.path.join(WORK, "bin")
LOGS = os.path.join(WORK, "logs")

DEFAULT_MINGW = r"C:\Qt\Tools\mingw1310_64\bin"
PER_TEST_TIMEOUT_S = 180

BASE = [
    "-std=c++17", "-Wall", "-Wextra",
    "-DDHCP_TEST_HOST", "-Dapp_main=esp_test_app_main",
    "-I.", "-Isrc", "-Itest/stubs",
]

# Tests that cannot be built on a PC, with the reason.  They need ESP-IDF
# headers that have no stand-in in test/stubs, or the board itself.
KNOWN_NOT_BUILT = {
    "test_auth": "needs the ESP-IDF mbedtls headers (base64.h)",
    "test_certstore": "needs the ESP-IDF mbedtls headers (error.h)",
    "test_config": "needs nvs_flash.h",
    "test_dhcp": "needs freertos/queue.h",
    "test_dns": "needs freertos/queue.h",
    "test_wifi": "needs driver/gpio.h, i.e. the board",
}

# Translation units a test needs but neither its own recipe nor the #include
# closure names -- the debt recorded in Plan/Memory.md section 9 (the recipes of
# these tests list too few files).  The symbols each entry supplies are named
# next to it, so a reader can check the entry instead of trusting it.
EXTRA_UNITS = {
    "test_filejson": ["src/files/TransferEngine.cpp",  # transferPhaseName()
                      "src/files/FileStatus.cpp",      # messageFor()
                      "src/core/JobRegistry.cpp",      # jobStateText(), JobInfo::percent()
                      "src/storage/PathUtil.cpp"],     # PathUtil::normalize/basename/parent
    "test_ptrprobe": ["src/dhcp/DnsMessage.cpp"],      # DnsMessage::readHeader/skipName/decodeName
    "test_transferengine": ["src/files/FileStatus.cpp"],  # messageFor()
    "test_dnsstatstore": ["src/core/ErrorLogCore.cpp"],
    "test_internalcache": ["src/dns/CacheFileReader.cpp"],
}

# app_main as the tests declare it: the symbol changes with the linkage, so the
# shim is picked by trying them in turn (see shim_order).
SHIMS = {
    "void_cpp": 'void esp_test_app_main();\n'
                'int main() { esp_test_app_main(); return 0; }\n',
    "void_c": 'extern "C" void esp_test_app_main(void);\n'
              'int main() { esp_test_app_main(); return 0; }\n',
    "int_c": 'extern "C" int esp_test_app_main(void);\n'
             'int main() { return esp_test_app_main(); }\n',
}


def parse_args(argv):
    """Return (verbose, mingw_bin, only) or None for an unknown switch."""
    verbose = False
    mingw = os.environ.get("DHCPSERVER_MINGW", DEFAULT_MINGW)
    only = []
    rest = list(argv)
    while rest:
        arg = rest.pop(0)
        if arg == "-v":
            verbose = True
        elif arg == "--mingw" and rest:
            mingw = rest.pop(0)
        elif arg.startswith("-"):
            return None
        else:
            only.append(arg)
    return verbose, mingw, only


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


def write_log(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def resolve_header(inc):
    for base in (REPO, os.path.join(REPO, "test")):
        cand = os.path.normpath(os.path.join(base, inc))
        if os.path.isfile(cand):
            return cand
    return None


def discovered_sources(text):
    """src/*.cpp the test needs, following its quoted includes."""
    sources = []
    seen = set()
    queue = [text]
    while queue:
        body = queue.pop()
        for inc in re.findall(r'#include\s+"([^"]+)"', body):
            header = resolve_header(inc)
            if header is None or header in seen:
                continue
            seen.add(header)
            stem, ext = os.path.splitext(header)
            cpp = stem + ".cpp"
            if os.path.isfile(cpp):
                name = rel(cpp)
                if name not in sources:
                    sources.append(name)
                queue.append(read(cpp))
            elif ext in (".h", ".hpp"):
                queue.append(read(header))
    return sources


def recipe(text):
    """Sources, libraries and include flags from the file's Build (MinGW) block."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if "Build (MinGW" in line:
            start = i
            break
    if start is None:
        return [], [], []

    block = []
    for line in lines[start:start + 30]:
        body = re.sub(r"^\s*(/\*\*?|\*/|\*|//)\s?", "", line).strip()
        if body.startswith("Build (MinGW"):
            body = body.split(":", 1)[1].strip() if ":" in body else ""
        if not body:
            if block:
                break
            continue
        if body.endswith("\\"):
            body = body[:-1]
        block.append(body)

    joined = " ".join(block)
    sources = [t for t in re.findall(r"[\w./\\-]+\.cpp", joined)
               if not t.endswith("host_main.cpp")]
    libs = [t for t in re.findall(r"-l\w+", joined)]
    includes = [t.replace(" ", "") for t in re.findall(r"-I\s*\S+", joined)]
    return sources, libs, includes


def shim_order(text):
    """Shims to try, best guess first (None means the file has its own main)."""
    order = []
    if re.search(r"\bint\s+main\s*\(", text):
        order.append(None)
    if re.search(r'extern\s+"C"\s+int\s+app_main', text):
        order.append("int_c")
    if re.search(r'extern\s+"C"\s+void\s+app_main', text) \
            or re.search(r'extern\s+"C"\s*\{', text):
        order.append("void_c")
    order.append("void_cpp")
    for name in ("void_c", "int_c"):
        if name not in order:
            order.append(name)
    return order


def first_error(text, limit=6):
    wanted = [ln.strip() for ln in text.splitlines()
              if "error:" in ln or "undefined reference" in ln]
    if not wanted:
        wanted = [ln.strip() for ln in text.splitlines() if ln.strip()]
    return " | ".join(wanted[-limit:])[:600]


def tail(text, limit=25):
    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    return "\n".join(lines[-limit:])


def main(argv):
    parsed = parse_args(argv)
    if parsed is None:
        print("unknown switch; see the file header for usage")
        return 2
    verbose, mingw, only = parsed
    gpp = os.path.join(mingw, "g++.exe")

    if not os.path.isfile(gpp):
        print("compiler not found: %s" % gpp)
        print("pass --mingw <dir> or set DHCPSERVER_MINGW")
        return 2

    os.makedirs(BIN, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    for name, body in SHIMS.items():
        with open(os.path.join(WORK, "shim_%s.cpp" % name), "w",
                  encoding="ascii", newline="\n") as fh:
            fh.write(body)

    def run(command, timeout=None):
        env = dict(os.environ)
        env["PATH"] = mingw + ";" + env.get("PATH", "")
        try:
            return subprocess.run(command, cwd=REPO, capture_output=True,
                                  text=True, errors="replace", timeout=timeout,
                                  env=env)
        except subprocess.TimeoutExpired:
            return None

    tests = sorted(f for f in os.listdir(os.path.join(REPO, "test"))
                   if f.startswith("test_") and f.endswith(".cpp")
                   and (not only or any(o in f for o in only)))

    version = subprocess.run([gpp, "-dumpversion"], capture_output=True,
                             text=True).stdout.strip()
    print("repository: %s" % REPO)
    print("compiler  : g++ %s" % version)
    print("work      : %s" % WORK)
    print("tests     : %d" % len(tests))
    print("")

    passed, not_built, failed = [], [], []
    for name in tests:
        test_path = os.path.join(REPO, "test", name)
        text = read(test_path)
        stem = name[:-4]
        exe = os.path.join(BIN, stem + ".exe")
        log = os.path.join(LOGS, stem + ".log")

        sources, libs, includes = recipe(text)
        # the recipe names the test file itself; it is added once, below
        sources = [s for s in sources
                   if os.path.basename(s.replace("\\", "/")) != name]
        for extra in discovered_sources(text) + EXTRA_UNITS.get(stem, []):
            if extra not in sources:
                sources.append(extra)
        test_rel = rel(test_path)

        # 1) syntax check of the whole unit set
        proc = run([gpp] + BASE + includes + ["-fsyntax-only", test_rel] + sources)
        if proc is None or proc.returncode != 0:
            err = "timeout" if proc is None else (proc.stderr or "")
            not_built.append((stem, first_error(err) if proc else "timeout"))
            write_log(log, "DOES NOT COMPILE\n%s\n" % tail(err))
            print("%-28s NOT BUILT (compile)" % stem)
            continue

        # 2) link, trying the entry-point shims
        built = None
        last = ""
        last_err = ""
        for shim in shim_order(text):
            command = [gpp] + BASE + includes + [test_rel] + sources
            if shim:
                command.append(os.path.join(WORK, "shim_%s.cpp" % shim))
            command += libs + ["-lws2_32", "-o", exe]
            proc = run(command)
            if proc is not None and proc.returncode == 0:
                built = shim or "own main"
                break
            last = "timeout" if proc is None else first_error(proc.stderr)
            last_err = "timeout" if proc is None else (proc.stderr or "")
        if built is None:
            not_built.append((stem, last))
            write_log(log, "DOES NOT LINK\n%s\n" % tail(last_err))
            print("%-28s NOT BUILT (link)" % stem)
            continue

        # 3) run
        start = time.time()
        proc = run([exe], timeout=PER_TEST_TIMEOUT_S)
        took = time.time() - start
        if proc is None:
            failed.append((stem, "timeout after %ds" % PER_TEST_TIMEOUT_S))
            print("%-28s TIMEOUT (entry: %s)" % (stem, built))
            continue
        out = (proc.stdout or "") + (proc.stderr or "")
        write_log(log, "entry point: %s\nexit code: %d\n%s"
                  % (built, proc.returncode, out))
        ok = proc.returncode == 0 and "PASSED" in out
        if ok:
            passed.append(stem)
            print("%-28s PASSED   (entry: %-9s %.1fs)" % (stem, built, took))
        else:
            failed.append((stem, "exit %d" % proc.returncode))
            print("%-28s FAILED   (entry: %s, exit %d)"
                  % (stem, built, proc.returncode))
        if verbose and not ok:
            print(out[-1500:])

    unexpected = [(s, w) for s, w in not_built if s not in KNOWN_NOT_BUILT]
    print("")
    print("PASSED     : %d" % len(passed))
    print("NOT BUILT  : %d (of them known: %d, unexpected: %d)"
          % (len(not_built), len(not_built) - len(unexpected), len(unexpected)))
    for stem, why in not_built:
        if stem not in KNOWN_NOT_BUILT:
            print("   - %-24s %s" % (stem, why[:150]))
        else:
            print("   - %-24s known: %s" % (stem, KNOWN_NOT_BUILT[stem]))
    print("RUN FAILED : %d" % len(failed))
    for stem, why in failed:
        print("   - %-24s %s" % (stem, why[:150]))

    if unexpected or failed:
        print("")
        print("RESULT: failures above")
        return 1
    print("")
    print("RESULT: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
