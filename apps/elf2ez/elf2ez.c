#include <stdio.h>
#include <string.h>

#include "fcntl.h"
#include "io.h"

#define ELF2EZ_OUTPUT_NAME_MAX 260u
#define ELF2EZ_COPY_BUFFER_SIZE 16384u

static int make_output_name(const char *input, char *output, unsigned output_size)
{
    const char *name;
    const char *dot = NULL;
    const char *p;
    unsigned prefix_len;

    if (input == NULL || input[0] == '\0' || output == NULL || output_size == 0)
        return 0;

    name = input;
    for (p = input; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
            dot = NULL;
        } else if (*p == '.') {
            dot = p;
        }
    }

    if (dot == name)
        dot = NULL;

    prefix_len = (unsigned)((dot != NULL ? dot : p) - input);
    if (prefix_len + sizeof(".exe") > output_size)
        return 0;

    memcpy(output, input, prefix_len);
    memcpy(output + prefix_len, ".exe", sizeof(".exe"));
    return 1;
}

static int is_yes_option(const char *arg)
{
    return arg != NULL && arg[0] == '/' &&
           (arg[1] == 'y' || arg[1] == 'Y') && arg[2] == '\0';
}

static int confirm_overwrite(const char *filename)
{
    int c;
    int tail;

    printf("Overwrite file: %s? (Y/N) ", filename);
    c = getchar();

    /*
     * stdin is the DOS console.  AH=3Fh may leave the rest of the entered
     * line (normally CR/LF) pending after the one-byte getchar(), so consume
     * it before returning control to the command interpreter.
     */
    do {
        tail = getchar();
    } while (tail >= 0 && tail != '\n');

    putchar('\n');
    return c == 'y' || c == 'Y';
}

static int copy_file(int input, int output)
{
    static unsigned char buffer[ELF2EZ_COPY_BUFFER_SIZE];

    for (;;) {
        int got = read(input, buffer, sizeof(buffer));
        unsigned done = 0;

        if (got < 0)
            return -1;
        if (got == 0)
            return 0;

        while (done < (unsigned)got) {
            int written = write(output, buffer + done, (unsigned)got - done);
            if (written <= 0)
                return 1;
            done += (unsigned)written;
        }
    }
}

int main(int argc, char **argv)
{
    char output_name[ELF2EZ_OUTPUT_NAME_MAX];
    const char *input_name = NULL;
    int overwrite = 0;
    int input;
    int output;
    int copy_result;
    int i;

    for (i = 1; i < argc; ++i) {
        if (is_yes_option(argv[i])) {
            overwrite = 1;
        } else if (input_name == NULL) {
            input_name = argv[i];
        } else {
            input_name = NULL;
            break;
        }
    }

    if (input_name == NULL) {
        printf("Usage: elf2ez [/y] <input-file>\n");
        return 1;
    }

    if (!make_output_name(input_name, output_name, sizeof(output_name))) {
        printf("Bad filename: %s\n", input_name);
        return 1;
    }

    input = open(input_name, O_RDONLY | O_BINARY);
    if (input < 0) {
        printf("Bad filename: %s\n", input_name);
        return 1;
    }

    if (!overwrite && access(output_name, 0) == 0 &&
        !confirm_overwrite(output_name)) {
        close(input);
        return 0;
    }

    output = open(output_name, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (output < 0) {
        close(input);
        printf("Unable to write file: %s\n", output_name);
        return 1;
    }

    copy_result = copy_file(input, output);
    close(input);

    if (close(output) < 0 && copy_result == 0)
        copy_result = 1;

    if (copy_result != 0) {
        remove(output_name);
        if (copy_result < 0)
            printf("Bad filename: %s\n", input_name);
        else
            printf("Unable to write file: %s\n", output_name);
        return 1;
    }

    return 0;
}
