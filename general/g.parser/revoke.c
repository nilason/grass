#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "proto.h"

#include <grass/spawn.h>
#include <grass/glocale.h>

int reinvoke_script(const struct context *ctx, const char *filename)
{
    struct Option *option;
    struct Flag *flag;

    /* Because shell from MINGW and CygWin converts all variables
     * to uppercase it was necessary to use uppercase variables.
     * Set both until all scripts are updated */
    for (flag = ctx->first_flag; flag; flag = flag->next_flag) {
        char buf1[16], buf2[16];

        snprintf(buf1, sizeof(buf1), "GIS_FLAG_%c", flag->key);
        snprintf(buf2, sizeof(buf2), "%d", flag->answer ? 1 : 0);
        G_putenv(buf1, buf2);

        snprintf(buf1, sizeof(buf1), "GIS_FLAG_%c", toupper(flag->key));
        G_debug(2, "set %s=%s", buf1, buf2);
        G_putenv(buf1, buf2);
    }

    for (option = ctx->first_option; option; option = option->next_opt) {
        char upper[4096];
        char *str;

        G_asprintf(&str, "GIS_OPT_%s", option->key);
        G_putenv(str, option->answer ? option->answer : "");

        strcpy(upper, option->key);
        G_str_to_upper(upper);
        G_asprintf(&str, "GIS_OPT_%s", upper);

        G_debug(2, "set %s=%s", str, option->answer ? option->answer : "");
        G_putenv(str, option->answer ? option->answer : "");
    }

#ifdef __MINGW32__
    {
        /* execlp() and _spawnlp ( _P_OVERLAY,..) do not work, they return
         * immediately and that breaks scripts running GRASS scripts
         * because they dont wait until GRASS script finished */
        /* execlp( "sh", "sh", filename, "@ARGS_PARSED@", NULL); */
        /* _spawnlp ( _P_OVERLAY, filename, filename, "@ARGS_PARSED@", NULL );
         */
        int ret;
        char *shell = getenv("GRASS_SH");

        if (shell == NULL)
            shell = "sh";
        ret = G_spawn(shell, shell, filename, "@ARGS_PARSED@", NULL);
        G_debug(1, "ret = %d", ret);
        if (ret == -1) {
            perror(_("G_spawn() failed"));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
#else
    execl(filename, filename, "@ARGS_PARSED@", NULL);

    perror(_("execl() failed"));
    return EXIT_FAILURE;
#endif
}
