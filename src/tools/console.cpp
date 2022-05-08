#include "console.h"

#ifdef SUPPORT_NCURSES
#include <signal.h>

#include <functional>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

#include <ncurses.h>
#include "ada/tools/text.h"

WINDOW* win_cmd;

std::stringstream buffer_cout;
std::stringstream buffer_cerr;
std::vector<std::string> buffer_cmd;

// currenct command state
std::string cmd;

size_t offset_cursor = 0;
size_t offset_buffer = 0;
size_t offset_cout = 0;
size_t tab_counter = 0;

void refresh_cmd_win() {
    werase(win_cmd);
    mvwprintw(win_cmd, 1, 1, "> %s", cmd.c_str() );
    box(win_cmd, 0, 0);
    wrefresh(win_cmd);
    wmove(win_cmd, 1, 3 + cmd.size() - offset_cursor);
};
#endif

void console_sigwinch_handler(int signal) {
    #ifdef SUPPORT_NCURSES
    endwin();
    erase();
    refresh();

    wresize(win_cmd, 3, COLS);
    tab_counter = 0;

    refresh_cmd_win();
    #endif
}

void console_init() {
    #ifdef SUPPORT_NCURSES

    initscr();
    raw();
    // start_color();
    cbreak();

    // Create windows
    win_cmd = newwin(3, COLS, 0, 0);

    // Capture Keys
    keypad(stdscr, true);
    scrollok(stdscr, true);
    noecho();

    // Capture all standard console OUT and ERR
    std::streambuf * old_cout = std::cout.rdbuf(buffer_cout.rdbuf());
    std::streambuf * old_cerr = std::cerr.rdbuf(buffer_cerr.rdbuf());

    refresh_cmd_win();
    #endif
}

void console_clear() {
    #ifdef SUPPORT_NCURSES
    cmd = "";
    buffer_cout.str("");
    buffer_cerr.str("");

    offset_cursor = 0;
    offset_buffer = 0;
    offset_cout = 0;
    tab_counter = 0;
    #endif
}

void console_refresh() {
    #ifdef SUPPORT_NCURSES
    erase();

    if (buffer_cerr.str().size() > 0)
        mvprintw(4, 0, "%s", buffer_cerr.str().c_str() );
    else 
        mvprintw(4, 0, "%s", buffer_cout.str().c_str() );
        
    refresh();
    refresh_cmd_win();
    #endif
}

bool console_getline(std::string& _cmd, CommandList& _commands, Sandbox& _sandbox) {
    #ifdef SUPPORT_NCURSES
    console_refresh();

    int ch = getch();

    if (ch == KEY_STAB || ch == '\t') 
        tab_counter++;
    else
        tab_counter = 0;

    if ( ch == '\n' || ch == KEY_ENTER || ch == KEY_EOL) {
        buffer_cout.str("");
        buffer_cerr.str("");
        buffer_cmd.push_back( cmd );
        offset_cursor = 0;
        offset_buffer = 0;

        _cmd = cmd;
        cmd = "";

        return true;
    }
    else if ( ch == KEY_BACKSPACE || ch == KEY_DC || ch == 127 ) {
        if (cmd.size() > offset_cursor)
            cmd.erase(cmd.end()-offset_cursor-1, cmd.end()-offset_cursor);
    }
    else if ( ch == KEY_STAB || ch == '\t') {
        buffer_cout.str("");
        buffer_cerr.str("");
        if (cmd.size() > 0) {
            if (cmd.find(',') == std::string::npos) {
                std::cout << "Suggestions:\n" << std::endl;

                std::string shorter_suggestion;

                for (size_t i = 0; i < _commands.size(); i++)
                    if ( _commands[i].trigger.rfind(cmd, 0) == 0) {
                        if (shorter_suggestion.size() == 0 || 
                            shorter_suggestion.size() > _commands[i].trigger.size())
                            shorter_suggestion = _commands[i].trigger;

                        std::cout << std::left << std::setw(27) << _commands[i].formula << " " << _commands[i].description << std::endl;
                    }

                for (UniformDataList::iterator it = _sandbox.uniforms.data.begin(); it != _sandbox.uniforms.data.end(); ++it) {
                    if (it->first.rfind(cmd, 0) == 0) {
                        if (shorter_suggestion.size() == 0 || 
                            shorter_suggestion.size() > it->first.size())
                            shorter_suggestion = it->first;

                        std::cout << it->first;

                        for (size_t i = 0; it->second.size; i++)
                            std::cout << ",<value>";
                        
                        std::cout << std::endl;
                    }
                }

                if (tab_counter > 1 && shorter_suggestion.size() > 0) {
                    cmd = shorter_suggestion;
                    tab_counter = 0;
                }
            }
            else {
                std::cout << "Use:" << std::endl;

                for (size_t i = 0; i < _commands.size(); i++)
                    if ( ada::beginsWith(cmd, _commands[i].trigger) )
                        std::cout << "      " << std::left << std::setw(16) << _commands[i].formula << "   " << _commands[i].description << std::endl;

                for (UniformDataList::iterator it = _sandbox.uniforms.data.begin(); it != _sandbox.uniforms.data.end(); ++it) {
                    if ( ada::beginsWith(cmd, it->first) ) {
                        std::cout << it->first;

                        for (size_t i = 0; it->second.size; i++)
                            std::cout << ",<value>";
                        
                        std::cout << std::endl;
                    }
                }

                std::cout << "\nNotes::" << std::endl;
                std::cout << "      - <values> between <...> brakets need to be change for and actual value" << std::endl;
                std::cout << "      - when words are separated by | you must choose one of the options, like: A|B|C" << std::endl;
                std::cout << "      * everything betwee [...] is optative" << std::endl;
            }
        }
    }
    else if ( ch == KEY_BREAK || ch == ' ') {
        cmd += ",";
    }
    else if ( ch == KEY_LEFT) {
        if (offset_cursor < cmd.size())
            offset_cursor++;
    }
    else if ( ch == KEY_RIGHT)
        offset_cursor--;
    else if ( ch == KEY_DOWN ) {
        if (buffer_cmd.size() > 0 && offset_buffer > 0) {
            offset_buffer--;
            offset_cursor = 0;

            if (offset_buffer == 0)
                cmd = "";
            else
                cmd = buffer_cmd[ buffer_cmd.size() - offset_buffer ];
        }
    }
    else if ( ch == KEY_UP ) {
        if (offset_buffer < buffer_cmd.size() - 1)
            offset_buffer++;
        offset_cursor = 0;
        if (offset_buffer < buffer_cmd.size() )
            cmd = buffer_cmd[ buffer_cmd.size() - 1 - offset_buffer ];
    }
    
    // else if ( ch == KEY_END || ch == KEY_EXIT || ch == 27 || ch == EOF) {
    //     keepRunnig = false;
    //     keepRunnig.store(false);
    //     break;
    // }
    else
        cmd.insert(cmd.end() - offset_cursor, 1, (char)ch );    
    #endif

    return false;
}

void console_draw_pct(float _pct) {
    #ifdef SUPPORT_NCURSES
    size_t lines, cols;
    getmaxyx(win_cmd, lines, cols);

    werase(win_cmd);
    box(win_cmd,0, 0);

    size_t l = (cols-4) * _pct;
    for (size_t i = 0; i < cols-4; i++)
        mvwprintw(win_cmd, 1, 2 + i, "%s", (i < l )? "#" : ".");

    wrefresh(win_cmd);
    #else

    // Delete previous line
    const std::string deleteLine = "\e[2K\r\e[1A";
    std::cout << deleteLine;
    
    std::cout << "// [ ";
    for (int i = 0; i < 50; i++) {
        if (i < pct/2) {
            std::cout << "#";
        }
        else {
            std::cout << ".";
        }
    }
    std::cout << " ] " << pct << "%" << std::endl;

    #endif
}

void console_end() {
    #ifdef SUPPORT_NCURSES
    endwin();
    #endif
}