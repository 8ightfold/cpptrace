#ifdef CPPTRACE_GET_SYMBOLS_WITH_ADDR2LINE

#include <cpptrace/cpptrace.hpp>
#include "symbols.hpp"
#include "../platform/common.hpp"
#include "../platform/utils.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if IS_LINUX || IS_APPLE
 #include <unistd.h>
 #include <sys/types.h>
 #include <sys/wait.h>
#endif

#include "../platform/object.hpp"

namespace cpptrace {
namespace detail {
namespace addr2line {
    #if IS_LINUX || IS_APPLE
    bool has_addr2line() {
        static std::mutex mutex;
        static bool has_addr2line = false;
        static bool checked = false;
        std::lock_guard<std::mutex> lock(mutex);
        if(!checked) {
            checked = true;
            // Detects if addr2line exists by trying to invoke addr2line --help
            constexpr int magic = 42;
            const pid_t pid = fork();
            if(pid == -1) { return false; }
            if(pid == 0) { // child
                close(STDOUT_FILENO);
                close(STDERR_FILENO); // atos --help writes to stderr
                #ifdef CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH
                #if !IS_APPLE
                execlp("addr2line", "addr2line", "--help", nullptr);
                #else
                execlp("atos", "atos", "--help", nullptr);
                #endif
                #else
                #ifndef CPPTRACE_ADDR2LINE_PATH
                #error "CPPTRACE_ADDR2LINE_PATH must be defined if CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH is not"
                #endif
                execl(CPPTRACE_ADDR2LINE_PATH, CPPTRACE_ADDR2LINE_PATH, "--help", nullptr);
                #endif
                _exit(magic);
            }
            int status;
            waitpid(pid, &status, 0);
            has_addr2line = WEXITSTATUS(status) == 0;
        }
        return has_addr2line;
    }

    struct pipe_t {
        union {
            struct {
                int read_end;
                int write_end;
            };
            int data[2];
        };
    };
    static_assert(sizeof(pipe_t) == 2 * sizeof(int), "Unexpected struct packing");

    std::string resolve_addresses(const std::string& addresses, const std::string& executable) {
        pipe_t output_pipe;
        pipe_t input_pipe;
        CPPTRACE_VERIFY(pipe(output_pipe.data) == 0);
        CPPTRACE_VERIFY(pipe(input_pipe.data) == 0);
        const pid_t pid = fork();
        if(pid == -1) { return ""; } // error? TODO: Diagnostic
        if(pid == 0) { // child
            dup2(output_pipe.write_end, STDOUT_FILENO);
            dup2(input_pipe.read_end, STDIN_FILENO);
            close(output_pipe.read_end);
            close(output_pipe.write_end);
            close(input_pipe.read_end);
            close(input_pipe.write_end);
            close(STDERR_FILENO); // TODO: Might be worth conditionally enabling or piping
            #ifdef CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH
            #if !IS_APPLE
            execlp("addr2line", "addr2line", "-e", executable.c_str(), "-f", "-C", "-p", nullptr);
            #else
            execlp("atos", "atos", "-o", executable.c_str(), "-fullPath", nullptr);
            #endif
            #else
            #ifndef CPPTRACE_ADDR2LINE_PATH
            #error "CPPTRACE_ADDR2LINE_PATH must be defined if CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH is not"
            #endif
            #if !IS_APPLE
            execl(
                CPPTRACE_ADDR2LINE_PATH,
                CPPTRACE_ADDR2LINE_PATH,
                "-e",
                executable.c_str(),
                "-f",
                "-C",
                "-p",
                nullptr
            );
            #else
            execl(
                CPPTRACE_ADDR2LINE_PATH,
                CPPTRACE_ADDR2LINE_PATH,
                "-o", executable.c_str(),
                "-fullPath",
                nullptr
            );
            #endif
            #endif
            _exit(1); // TODO: Diagnostic?
        }
        CPPTRACE_VERIFY(write(input_pipe.write_end, addresses.data(), addresses.size()) != -1);
        close(input_pipe.read_end);
        close(input_pipe.write_end);
        close(output_pipe.write_end);
        std::string output;
        constexpr int buffer_size = 4096;
        char buffer[buffer_size];
        size_t count = 0;
        while((count = read(output_pipe.read_end, buffer, buffer_size)) > 0) {
            output.insert(output.end(), buffer, buffer + count);
        }
        // TODO: check status from addr2line?
        waitpid(pid, nullptr, 0);
        return output;
    }
    #elif IS_WINDOWS
    bool has_addr2line() {
        static std::mutex mutex;
        static bool has_addr2line = false;
        static bool checked = false;
        std::lock_guard<std::mutex> lock(mutex);
        if(!checked) {
            // TODO: Popen is a hack. Implement properly with CreateProcess and pipes later.
            checked = true;
            #ifdef CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH
            FILE* p = popen("addr2line --version", "r");
            #else
            #ifndef CPPTRACE_ADDR2LINE_PATH
            #error "CPPTRACE_ADDR2LINE_PATH must be defined if CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH is not"
            #endif
            FILE* p = popen(CPPTRACE_ADDR2LINE_PATH " --version", "r");
            #endif
            if(p) {
                has_addr2line = pclose(p) == 0;
            }
        }
        return has_addr2line;
    }

    std::string resolve_addresses(const std::string& addresses, const std::string& executable) {
        // TODO: Popen is a hack. Implement properly with CreateProcess and pipes later.
        ///fprintf(stderr, ("addr2line -e " + executable + " -fCp " + addresses + "\n").c_str());
        #ifdef CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH
        FILE* p = popen(("addr2line -e \"" + executable + "\" -fCp " + addresses).c_str(), "r");
        #else
        #ifndef CPPTRACE_ADDR2LINE_PATH
        #error "CPPTRACE_ADDR2LINE_PATH must be defined if CPPTRACE_ADDR2LINE_SEARCH_SYSTEM_PATH is not"
        #endif
        FILE* p = popen(
            (CPPTRACE_ADDR2LINE_PATH " -e \"" + executable + "\" -fCp " + addresses).c_str(),
            "r"
        );
        #endif
        std::string output;
        constexpr int buffer_size = 4096;
        char buffer[buffer_size];
        size_t count = 0;
        while((count = fread(buffer, 1, buffer_size, p)) > 0) {
            output.insert(output.end(), buffer, buffer + count);
        }
        pclose(p);
        ///fprintf(stderr, "%s\n", output.c_str());
        return output;
    }
    #endif

    void update_trace(const std::string& line, size_t entry_index, const collated_vec& entries_vec) {
        #if !IS_APPLE
        // Result will be of the form "<symbol> at path:line"
        // The path may be ?? if addr2line cannot resolve, line may be ?
        // Edge cases:
        // ?? ??:0
        // symbol :?
        const std::size_t at_location = line.find(" at ");
        std::size_t symbol_end;
        std::size_t filename_start;
        if(at_location != std::string::npos) {
            symbol_end = at_location;
            filename_start = at_location + 4;
        } else {
            CPPTRACE_VERIFY(line.find("?? ") == 0, "Unexpected edge case while processing addr2line output");
            symbol_end = 2;
            filename_start = 3;
        }
        auto symbol = line.substr(0, symbol_end);
        auto colon = line.rfind(':');
        CPPTRACE_VERIFY(colon != std::string::npos);
        CPPTRACE_VERIFY(colon >= filename_start); // :? to deal with "symbol :?" edge case
        auto filename = line.substr(filename_start, colon - filename_start);
        auto line_number = line.substr(colon + 1);
        if(line_number != "?") {
            entries_vec[entry_index].second.get().line = std::stoi(line_number);
        }
        if(!filename.empty() && filename != "??") {
            entries_vec[entry_index].second.get().filename = filename;
        }
        if(!symbol.empty()) {
            entries_vec[entry_index].second.get().symbol = symbol;
        }
        #else
        // Result will be of the form "<symbol> (in <object name>) (file:line)"
        // The symbol may just be the given address if atos can't resolve it
        // Examples:
        // trace() (in demo) (demo.cpp:8)
        // 0x100003b70 (in demo)
        // 0xffffffffffffffff
        // foo (in bar) + 14
        // I'm making some assumptions here. Support may need to be improved later. This is tricky output to
        // parse.
        const std::size_t in_location = line.find(" (in ");
        if(in_location == std::string::npos) {
            // presumably the 0xffffffffffffffff case
            return;
        }
        const std::size_t symbol_end = in_location;
        entries_vec[entry_index].second.get().symbol = line.substr(0, symbol_end);
        const std::size_t obj_end = line.find(")", in_location);
        CPPTRACE_VERIFY(
            obj_end != std::string::npos,
            "Unexpected edge case while processing addr2line/atos output"
        );
        const std::size_t filename_start = line.find(") (", obj_end);
        if(filename_start == std::string::npos) {
            // presumably something like 0x100003b70 (in demo) or foo (in bar) + 14
            return;
        }
        const std::size_t filename_end = line.find(":", filename_start);
        CPPTRACE_VERIFY(
            filename_end != std::string::npos,
            "Unexpected edge case while processing addr2line/atos output"
        );
        entries_vec[entry_index].second.get().filename = line.substr(
            filename_start + 3,
            filename_end - filename_start - 3
        );
        const std::size_t line_start = filename_end + 1;
        const std::size_t line_end = line.find(")", filename_end);
        CPPTRACE_VERIFY(
            line_end == line.size() - 1,
            "Unexpected edge case while processing addr2line/atos output"
        );
        entries_vec[entry_index].second.get().line = std::stoi(line.substr(line_start, line_end - line_start));
        #endif
    }

    std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames) {
        // TODO: Refactor better
        std::vector<stacktrace_frame> trace(frames.size(), stacktrace_frame { 0, 0, UINT_LEAST32_MAX, "", "" });
        for(size_t i = 0; i < frames.size(); i++) {
            trace[i].address = frames[i].raw_address;
            // Set what is known for now, and resolutions from addr2line should overwrite
            trace[i].filename = frames[i].obj_path;
            trace[i].symbol = frames[i].symbol;
        }
        if(has_addr2line()) {
            const auto entries = collate_frames(frames, trace);
            for(const auto& entry : entries) {
                const auto& object_name = entry.first;
                const auto& entries_vec = entry.second;
                // You may ask why it'd ever happen that there could be an empty entries_vec array, if there're
                // no addresses why would get_addr2line_targets do anything? The reason is because if things in
                // get_addr2line_targets fail it will silently skip. This is partly an optimization but also an
                // assertion below will fail if addr2line is given an empty input.
                if(entries_vec.empty()) {
                    continue;
                }
                std::string address_input;
                for(const auto& pair : entries_vec) {
                    address_input += to_hex(pair.first.get().obj_address);
                    #if !IS_WINDOWS
                        address_input += '\n';
                    #else
                        address_input += ' ';
                    #endif
                }
                auto output = split(trim(resolve_addresses(address_input, object_name)), "\n");
                CPPTRACE_VERIFY(output.size() == entries_vec.size());
                for(size_t i = 0; i < output.size(); i++) {
                    update_trace(output[i], i, entries_vec);
                }
            }
        }
        return trace;
    }
}
}
}

#endif
