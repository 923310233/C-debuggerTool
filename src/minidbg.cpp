#include "debugger.hpp"
#include "registers.hpp"
#include "breakpoint.hpp"

#include "linenoise.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <map>
#include <error.h>
#include <set>

using namespace minidbg;

static int last = 0;
static std::set<int> success_set;
static std::set<int> fail_set;


symbol_type to_symbol_type(elf::stt sym) {
    switch (sym) {
        case elf::stt::notype:
            return symbol_type::notype;
        case elf::stt::object:
            return symbol_type::object;
        case elf::stt::func:
            return symbol_type::func;
        case elf::stt::section:
            return symbol_type::section;
        case elf::stt::file:
            return symbol_type::file;
        default:
            return symbol_type::notype;
    }
};

std::vector<symbol> debugger::lookup_symbol(const std::string &name) {
    std::vector<symbol> syms;

    for (auto &sec : m_elf.sections()) {
        if (sec.get_hdr().type != elf::sht::symtab && sec.get_hdr().type != elf::sht::dynsym)
            continue;

        for (auto sym : sec.as_symtab()) {
            if (sym.get_name() == name) {
                auto &d = sym.get_data();
                syms.push_back(symbol{to_symbol_type(d.type()), sym.get_name(), d.value});
            }
        }
    }

    return syms;
}

uint64_t debugger::get_pc() {
    return get_register_value(m_pid, reg::rip);
}

void debugger::set_pc(uint64_t pc) {
    set_register_value(m_pid, reg::rip, pc);
}

dwarf::die debugger::get_function_from_pc(uint64_t pc) {
    for (auto &cu : m_dwarf.compilation_units()) {
        if (die_pc_range(cu.root()).contains(pc)) {
            for (const auto &die : cu.root()) {
                if (die.tag == dwarf::DW_TAG::subprogram) {
                    if (die_pc_range(die).contains(pc)) {
                        return die;
                    }
                }
            }
        }
    }

    throw std::out_of_range{"Cannot find function"};
}

dwarf::line_table::iterator debugger::get_line_entry_from_pc(uint64_t pc) {
    for (auto &cu : m_dwarf.compilation_units()) {
        if (die_pc_range(cu.root()).contains(pc)) {
            auto &lt = cu.get_line_table();
            auto it = lt.find_address(pc);
            if (it == lt.end()) {
                throw std::out_of_range{"Cannot find line entry"};
            } else {
                return it;
            }
        }
    }

    throw std::out_of_range{"Cannot find line entry"};
}

void debugger::run() {
    int wait_status;
    auto options = 0;
    waitpid(m_pid, &wait_status, options);

    char *line = nullptr;
    while ((line = linenoise("minidbg> ")) != nullptr) {
        handle_command(line);
        linenoiseHistoryAdd(line);
        linenoiseFree(line);
    }
}

void debugger::print_source(const std::string &file_name, unsigned line, unsigned n_lines_context) {
    std::ifstream file{file_name};
    auto start_line = line <= n_lines_context ? 1 : line - n_lines_context;
    auto end_line = line + n_lines_context + (line < n_lines_context ? n_lines_context - line : 0) + 1;

    char c{};
    auto current_line = 1u;
    while (current_line != start_line && file.get(c)) {
        if (c == '\n') {
            ++current_line;
        }
    }
    std::cout << (current_line == line ? "> " : "  ");
    while (current_line <= end_line && file.get(c)) {
        std::cout << c;
        if (c == '\n') {
            ++current_line;
            std::cout << (current_line == line ? "> " : "  ");
        }
    }
    std::cout << std::endl;
}

bool is_prefix(const std::string &s, const std::string &of) {
    if (s.size() > of.size()) return false;
    return std::equal(s.begin(), s.end(), of.begin());
}

bool is_suffix(const std::string &s, const std::string &of) {
    if (s.size() > of.size()) return false;
    auto diff = of.size() - s.size();
    return std::equal(s.begin(), s.end(), of.begin() + diff);
}

siginfo_t debugger::get_signal_info() {
    siginfo_t info;
    ptrace(PTRACE_GETSIGINFO, m_pid, nullptr, &info);
    return info;
}

void debugger::handle_sigtrap(siginfo_t info) {
    switch (info.si_code) {
        //one of these will be set if a breakpoint was hit
        case SI_KERNEL:
        case TRAP_BRKPT: {
            set_pc(get_pc() - 1);
            //std::cout << "Hit breakpoint at address 0x" << std::hex << get_pc() << std::endl;
            auto line_entry = get_line_entry_from_pc(get_pc());


            print_source_advice(line_entry->file->path, line_entry->line);
            return;
        }
            //this will be set if the signal was sent by single stepping
        case TRAP_TRACE:
            return;
        default:
            std::cout << "Unknown SIGTRAP code " << info.si_code << std::endl;
            return;
    }
}

void debugger::wait_for_signal() {
    int wait_status;
    auto options = 0;
    waitpid(m_pid, &wait_status, options);

    auto siginfo = get_signal_info();

    switch (siginfo.si_signo) {
        case SIGTRAP:
            handle_sigtrap(siginfo);
            break;
        case SIGSEGV:
            std::cout << "Yay, segfault. Reason: " << siginfo.si_code << std::endl;
            break;
        default:
            std::cout << "Got signal " << strsignal(siginfo.si_signo) << std::endl;
    }
}

void debugger::continue_execution() {
    step_over_breakpoint();
    ptrace(PTRACE_CONT, m_pid, nullptr, nullptr);
    wait_for_signal();
}

void debugger::single_step_instruction() {
    ptrace(PTRACE_SINGLESTEP, m_pid, nullptr, nullptr);
    wait_for_signal();
}

void debugger::step_over_breakpoint() {
    if (m_breakpoints.count(get_pc())) {
        auto &bp = m_breakpoints[get_pc()];
        if (bp.is_enabled()) {
            bp.disable();
            single_step_instruction();
            bp.enable();
        }
    }
}

void debugger::step_over() {
    auto func = get_function_from_pc(get_pc());
    auto func_entry = at_low_pc(func);
    auto func_end = at_high_pc(func);

    auto line = get_line_entry_from_pc(func_entry);
    auto start_line = get_line_entry_from_pc(get_pc());

    std::vector<std::intptr_t> breakpoints_to_remove{};

    //set breakpoints on all lines apart from the current one if they don't already have one set
    while (line->address < func_end) {
        if (line->address != start_line->address && !m_breakpoints.count(line->address)) {
            set_breakpoint_at_address(line->address);
            breakpoints_to_remove.push_back(line->address);
        }
        ++line;
    }

    //set breakpoint on return address
    auto frame_pointer = get_register_value(m_pid, reg::rbp);
    auto return_address = read_memory(frame_pointer + 8);
    if (!m_breakpoints.count(return_address)) {
        set_breakpoint_at_address(return_address);
        breakpoints_to_remove.push_back(return_address);
    }

    continue_execution();

    for (auto addr : breakpoints_to_remove) {
        remove_breakpoint(addr);
    }
}

void debugger::step_out() {
    auto frame_pointer = get_register_value(m_pid, reg::rbp);
    auto return_address = read_memory(frame_pointer + 8);

    bool should_remove_breakpoint = false;
    if (!m_breakpoints.count(return_address)) {
        set_breakpoint_at_address(return_address);
        should_remove_breakpoint = true;
    }

    continue_execution();

    if (should_remove_breakpoint) {
        remove_breakpoint(return_address);
    }
}

void debugger::step_in() {
    auto line = get_line_entry_from_pc(get_pc())->line;

    while (get_line_entry_from_pc(get_pc())->line == line) {
        single_step_instruction_with_breakpoint_check();
    }

    auto line_entry = get_line_entry_from_pc(get_pc());
    print_source(line_entry->file->path, line_entry->line);
}

void debugger::single_step_instruction_with_breakpoint_check() {
    //first, check to see if we need to disable and enable a breakpoint
    if (m_breakpoints.count(get_pc())) {
        step_over_breakpoint();
    } else {
        single_step_instruction();
    }

    auto line_entry = get_line_entry_from_pc(get_pc());

    print_source_advice(line_entry->file->path, line_entry->line);

}

void debugger::remove_breakpoint(std::intptr_t addr) {
    if (m_breakpoints.at(addr).is_enabled()) {
        m_breakpoints.at(addr).disable();
    }
    m_breakpoints.erase(addr);
}

void debugger::set_breakpoint_at_address(std::intptr_t addr) {
    //std::cout << "Set breakpoint at address 0x" << std::hex << addr << std::endl;
    breakpoint bp{m_pid, addr};
    bp.enable();
    m_breakpoints[addr] = bp;
}

void debugger::set_breakpoint_at_function(const std::string &name) {
    for (const auto &cu : m_dwarf.compilation_units()) {
        for (const auto &die : cu.root()) {
            if (die.has(dwarf::DW_AT::name) && at_name(die) == name) {
                auto low_pc = at_low_pc(die);
                auto entry = get_line_entry_from_pc(low_pc);
                ++entry; //skip prologue
                set_breakpoint_at_address(entry->address);
            }
        }
    }
}

void debugger::set_breakpoint_at_source_line(const std::string &file, unsigned line) {
    for (const auto &cu : m_dwarf.compilation_units()) {
        if (is_suffix(file, at_name(cu.root()))) {
            const auto &lt = cu.get_line_table();

            for (const auto &entry : lt) {
                if (entry.is_stmt && entry.line == line) {
                    set_breakpoint_at_address(entry.address);
                    return;
                }
            }
        }
    }
}

void debugger::dump_registers() {
    for (const auto &rd : g_register_descriptors) {
        std::cout << rd.name << " 0x"
                  << std::setfill('0') << std::setw(16) << std::hex << get_register_value(m_pid, rd.r) << std::endl;
    }
}

class ptrace_expr_context : public dwarf::expr_context {
public:
    ptrace_expr_context(pid_t pid) : m_pid{pid} {}

    dwarf::taddr reg(unsigned regnum) override {
        return get_register_value_from_dwarf_register(m_pid, regnum);
    }

    dwarf::taddr pc() override {
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, m_pid, nullptr, &regs);
        return regs.rip;
    }

    dwarf::taddr deref_size(dwarf::taddr address, unsigned size) override {
        //TODO take into account size
        return ptrace(PTRACE_PEEKDATA, m_pid, address, nullptr);
    }

private:
    pid_t m_pid;
};

void debugger::read_variables() {
    using namespace dwarf;

    auto func = get_function_from_pc(get_pc());

    for (const auto &die : func) {
        if (die.tag == DW_TAG::variable) {
            auto loc_val = die[DW_AT::location];

            //only supports exprlocs for now
            if (loc_val.get_type() == value::type::exprloc) {
                ptrace_expr_context context{m_pid};
                auto result = loc_val.as_exprloc().evaluate(&context);

                switch (result.location_type) {
                    case expr_result::type::address: {
                        auto value = read_memory(result.value);
                        std::cout << at_name(die) << " (0x" << std::hex << result.value << ") = " << value << std::endl;
                        break;
                    }

                    case expr_result::type::reg: {
                        auto value = get_register_value_from_dwarf_register(m_pid, result.value);
                        std::cout << at_name(die) << " (reg " << result.value << ") = " << value << std::endl;
                        break;
                    }

                    default:
                        throw std::runtime_error{"Unhandled variable location"};
                }
            } else {
                throw std::runtime_error{"Unhandled variable location"};
            }
        }
    }
}

uint64_t debugger::read_memory(uint64_t address) {
    return ptrace(PTRACE_PEEKDATA, m_pid, address, nullptr);
}

void debugger::write_memory(uint64_t address, uint64_t value) {
    ptrace(PTRACE_POKEDATA, m_pid, address, value);
}

std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> out{};
    std::stringstream ss{s};
    std::string item;

    while (std::getline(ss, item, delimiter)) {
        out.push_back(item);
    }

    return out;
}

void debugger::print_backtrace() {
    auto current_func = get_function_from_pc(get_pc());

    auto output_frame = [frame_number = 0](auto &&func) mutable {
        std::cout << "frame #" << frame_number++ << ": 0x" << dwarf::at_low_pc(func)
                  << ' ' << dwarf::at_name(func) << std::endl;
    };

    output_frame(current_func);

    auto frame_pointer = get_register_value(m_pid, reg::rbp);
    auto return_address = read_memory(frame_pointer + 8);
    while (dwarf::at_name(current_func) != "main") {
        current_func = get_function_from_pc(return_address);
        output_frame(current_func);
        frame_pointer = read_memory(frame_pointer);
        return_address = read_memory(frame_pointer + 8);
    }
}

void debugger::handle_command(const std::string &line) {
    auto args = split(line, ' ');
    auto command = args[0];

    if (is_prefix(command, "cont")) {
        continue_execution();
    } else if (is_prefix(command, "break")) {
        if (args[1][0] == '0' && args[1][1] == 'x') {
            std::string addr{args[1], 2};
            set_breakpoint_at_address(std::stol(addr, 0, 16));
        } else if (args[1].find(':') != std::string::npos) {
            auto file_and_line = split(args[1], ':');
            set_breakpoint_at_source_line(file_and_line[0], std::stoi(file_and_line[1]));
        } else {
            set_breakpoint_at_function(args[1]);
        }
    } else if (is_prefix(command, "step")) {
        step_in();
    } else if (is_prefix(command, "next")) {
        step_over();
    } else if (is_prefix(command, "finish")) {
        step_out();
    } else if (is_prefix(command, "stepi")) {
        single_step_instruction_with_breakpoint_check();
        auto line_entry = get_line_entry_from_pc(get_pc());
        print_source(line_entry->file->path, line_entry->line);
    } else if (is_prefix(command, "status")) {
        auto line_entry = get_line_entry_from_pc(get_pc());
        print_source(line_entry->file->path, line_entry->line);
    } else if (is_prefix(command, "register")) {
        if (is_prefix(args[1], "dump")) {
            dump_registers();
        } else if (is_prefix(args[1], "read")) {
            std::cout << get_register_value(m_pid, get_register_from_name(args[2])) << std::endl;
        } else if (is_prefix(args[1], "write")) {
            std::string val{args[3], 2}; //assume 0xVAL
            set_register_value(m_pid, get_register_from_name(args[2]), std::stol(val, 0, 16));
        }
    } else if (is_prefix(command, "memory")) {
        std::string addr{args[2], 2}; //assume 0xADDRESS

        if (is_prefix(args[1], "read")) {
            std::cout << std::hex << read_memory(std::stol(addr, 0, 16)) << std::endl;
        }
        if (is_prefix(args[1], "write")) {
            std::string val{args[3], 2}; //assume 0xVAL
            write_memory(std::stol(addr, 0, 16), std::stol(val, 0, 16));
        }
    } else if (is_prefix(command, "variables")) {
        read_variables();
    } else if (is_prefix(command, "backtrace")) {
        print_backtrace();
    } else if (is_prefix(command, "symbol")) {
        auto syms = lookup_symbol(args[1]);
        for (auto &&s : syms) {
            std::cout << s.name << ' ' << to_string(s.type) << " 0x" << std::hex << s.addr << std::endl;
        }
    } else {
        std::cerr << "Unknown command\n";
    }
}

void execute_debugee(const std::string &prog_name) {
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
        std::cerr << "Error in ptrace\n";
        return;
    }
    execl(prog_name.c_str(), prog_name.c_str(), nullptr);
}


int CountLines(const char *filename) {
    std::ifstream ReadFile;
    int n = 0;
    std::string tmp;
    ReadFile.open(filename, std::ios::in);//ios::in 表示以只读的方式读取文件
    if (ReadFile.fail())//文件打开失败:返回0
    {
        return 0;
    } else//文件存在
    {
        while (getline(ReadFile, tmp, '\n')) {
            n++;
        }
        ReadFile.close();
        return n;
    }
}

std::string ReadLine(const char *filename, int line) {
    int lines, i = 0;
    std::string temp;
    std::fstream file;
    file.open(filename, std::ios::in);
    lines = CountLines(filename);

    if (line <= 0) {
        return "Error 1: 行数错误，不能为0或负数。";
    }
    if (file.fail()) {
        return "Error 2: 文件不存在。";
    }
    if (line > lines) {
        return "Error 3: 行数超出文件长度。";
    }
    while (getline(file, temp) && i < line - 1) {
        i++;
    }
    file.close();
    return temp;
}


void debugger::print_source_advice(const std::string &file_name, unsigned line, unsigned n_lines_context) {

    if (last == line)return;

    last = line;


    //std::ifstream file {file_name};
    auto start_line = line <= n_lines_context ? 1 : line - n_lines_context;
    auto end_line = line + n_lines_context + (line < n_lines_context ? n_lines_context - line : 0) + 1;

    std::cout << "Now Execute--" << line << "Line" << "\n";

    auto ite = source_map.find(line);
    if (ite == source_map.end()) {
        source_map.insert(std::pair<int, int>(line, 1));
    } else {
        ite->second = ite->second + 1;
    }


    std::cout << ReadLine(file_name.c_str(), line);

    std::cout << std::endl;
}


void debugger::step_in_advice() {
    auto line = get_line_entry_from_pc(get_pc())->line;

    while (get_line_entry_from_pc(get_pc())->line == line) {
        single_step_instruction_with_breakpoint_check();
    }

    auto line_entry = get_line_entry_from_pc(get_pc());
}


void debugger::runAdvice() {
    int wait_status;
    auto options = 0;
    waitpid(m_pid, &wait_status, options);

    set_breakpoint_at_function("main");
    continue_execution();

    while (true) {
        try {
            step_in_advice();
        } catch (std::out_of_range) {
            continue_execution();
            break;
        }
    }
    std::cout << "\n";
    std::cout << "Conclusion:   \n";

    std::map<int, int>::iterator iter;
    iter = source_map.begin();
    while (iter != source_map.end()) {
        std::cout << "Line " << iter->first << "was executed for" << " : " << iter->second << " TIMES" << "\n";
        iter++;
    }
    last = 0;
}

std::vector<std::string> split(const char *s, const char *delim) {
    std::vector<std::string> result;
    if (s && strlen(s)) {
        int len = strlen(s);
        char *src = new char[len + 1];
        strcpy(src, s);
        src[len] = '\0';
        char *tokenptr = strtok(src, delim);
        while (tokenptr != NULL) {
            std::string tk = tokenptr;
            result.emplace_back(tk);
            tokenptr = strtok(NULL, delim);
        }
        delete[] src;
    }
    return result;
}


int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "Program name not specified";
        return -1;
    }

    auto prog = argv[1];

    char *filePath = argv[2];
    std::ifstream file;
    file.open(filePath, std::ios::in);
    int count = 1;
    if (!file.is_open())return 0;
    std::string strLine;
    while (getline(file, strLine)) {
        const std::vector<std::string> &vec = split(strLine.c_str(), " ");
        auto pid = fork();
        if (pid == 0) {
            if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
                std::cerr << "Error in ptrace\n";
                return 0;
            }
            execl(prog, vec[0].c_str(), vec[1].c_str(),
                  vec[2].c_str(), vec[3].c_str(), vec[4].c_str(),
                  vec[5].c_str(), vec[6].c_str(), vec[7].c_str(),
                  vec[8].c_str(), nullptr);
        } else if (pid >= 1) {
            //parent
            debugger dbg{prog, pid};
            dbg.runAdvice();

            std::cout << "\n";
            std::cout << "result----- \n";

            if (pid == 0) {
                execl(prog, vec[0].c_str(), vec[1].c_str(),
                      vec[2].c_str(), vec[3].c_str(), vec[4].c_str(),
                      vec[5].c_str(), vec[6].c_str(), vec[7].c_str(),
                      vec[8].c_str(), nullptr);
                std::cout << "\n";
            }
            else if (pid >= 1) {
                if (waitpid(pid, NULL, 0) < 0) {
                    //printf("%d\n", error);
                }
                getline(file, strLine);//5.txt
                std::cout << "correct answer:" << strLine << std::endl;
                char *path = "1.txt";
                std::ifstream infile;
                infile.open(path, std::ios::in);
                std::string line;
                getline(infile, line);
                std::cout << "test answer: " << line << "\n";
                if(line==strLine){
                    std::cout<<"success-----\n";
                    std::map<int, int>::iterator iter;
                    iter = dbg.source_map.begin();
                    while (iter != dbg.source_map.end()) {
                        success_set.insert(iter->first);
                        iter++;
                    }
                    dbg.source_map.clear();
                } else if(line!=strLine){
                    std::cout<<"fail-----\n";
                    std::map<int, int>::iterator iter;
                    iter = dbg.source_map.begin();
                    while (iter != dbg.source_map.end()) {
                        fail_set.insert(iter->first);
                        iter++;
                    }
                    dbg.source_map.clear();
                }


            }
        }
    }

 std::cout<<"ANALYZE :  \n";
    std::set<int>::iterator it;
    for(it=fail_set.begin ();it!=fail_set.end ();it++)
    {
        if(success_set.find(*it)==success_set.end()){
            std::cout<<"Line :"<<*it<<" is likely to be a fault\n";
        }
    }


}
