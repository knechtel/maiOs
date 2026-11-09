#ifndef MAIOS_MICROPYTHON_PORT_H
#define MAIOS_MICROPYTHON_PORT_H

void maios_python_repl(void);
int maios_python_run_file(const char *filename);
int maios_python_read_char(void);
void maios_python_write_char(char value);
void maios_python_move_cursor_back(unsigned int count);
void maios_python_erase_line_from_cursor(unsigned int count);

#endif
