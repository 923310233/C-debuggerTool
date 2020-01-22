#ifndef MINIDBG_DEBUGGER_HPP
#define MINIDBG_DEBUGGER_HPP

#include <unordered_map>
#include <cstdint>
#include <signal.h>
#include <fcntl.h>

#include "dwarf/dwarf++.hh"
#include "elf/elf++.hh"

#include "breakpoint.hpp"

namespace minidbg {
    enum class symbol_type {
        notype,            // No type (e.g., absolute symbol)
        object,            // Data object
        func,              // Function entry point
        section,           // Symbol is associated with a section
        file,              // Source file associated with the
    };                     // object file

    std::string to_string (symbol_type st) {
        switch (st) {
        case symbol_type::notype: return "notype";
        case symbol_type::object: return "object";
        case symbol_type::func: return "func";
        case symbol_type::section: return "section";
        case symbol_type::file: return "file";
        }
    }

    struct symbol {
        symbol_type type;
        std::string name;
        std::uintptr_t addr;
    };

    class debugger {
    public:
        debugger (std::string prog_name, pid_t pid)
            : m_prog_name{std::move(prog_name)}, m_pid{pid} {
                auto fd = open(m_prog_name.c_str(), O_RDONLY);

                m_elf = elf::elf{elf::create_mmap_loader(fd)};
                m_dwarf = dwarf::dwarf{dwarf::elf::create_loader(m_elf)};
            }

        void run();
        void dump_registers();
        auto read_memory(uint64_t address) -> uint64_t;
        void write_memory(uint64_t address, uint64_t value);
        void print_backtrace();
        void read_variables();
        void continue_execution();
        void single_step_instruction_with_breakpoint_check();
        void step_in();
        void step_out();
        void step_over();
        auto get_signal_info() -> siginfo_t;
        void remove_breakpoint(std::intptr_t addr);
        void set_breakpoint_at_function(const std::string& name);
        void set_breakpoint_at_address(std::intptr_t addr);
        void set_breakpoint_at_source_line(const std::string& file, unsigned line);
        void print_source(const std::string& file_name, unsigned line, unsigned n_lines_context=2);
        auto lookup_symbol(const std::string&) -> std::vector<symbol>;

        std::map<int,int> source_map;
        void runAdvice();
        void step_in_advice();
        void print_source_advice(const std::string& file_name, unsigned line, unsigned n_lines_context=2);


    private:
        void single_step_instruction(); //single step without checking breakpoints
        void step_over_breakpoint();
        void handle_command(const std::string& line);
        void handle_sigtrap(siginfo_t info);
        void wait_for_signal();
        auto get_pc() -> uint64_t;
        void set_pc(uint64_t pc);
        auto get_line_entry_from_pc(uint64_t pc) -> dwarf::line_table::iterator;
        auto get_function_from_pc(uint64_t pc) -> dwarf::die;

        std::string m_prog_name;
        pid_t m_pid;
        dwarf::dwarf m_dwarf;
        elf::elf m_elf;
        std::unordered_map<std::intptr_t,breakpoint> m_breakpoints;



    };
}

#endif
