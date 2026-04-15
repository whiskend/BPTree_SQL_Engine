#ifndef CLI_H
#define CLI_H

typedef struct {
    char *db_dir;
    char *sql_file;
    int help_requested;
} CliOptions;

int parse_cli_args(int argc, char **argv, CliOptions *out_options);
void print_usage(const char *program_name);

#endif
