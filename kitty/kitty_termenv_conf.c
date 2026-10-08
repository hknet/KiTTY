/*
 * kitty_termenv_conf.c - the environment list a connection sends, read from
 * a Conf (CONF_environmt) through the merge rule in kitty_termenv.c.
 * See kitty_termenv.h.
 */
#include "putty.h"
#include "kitty_termenv.h"

KittyTermEnvList *kitty_termenv_list_new(Conf *conf)
{
    KittyTermEnvList *list = snew(KittyTermEnvList);
    KittyTermEnvVar *vars;
    const char **names, **values;
    char *key, *val;
    size_t n = 0, i;

    for (val = conf_get_str_strs(conf, CONF_environmt, NULL, &key);
         val != NULL;
         val = conf_get_str_strs(conf, CONF_environmt, key, &key))
        n++;
    names = snewn(n + 1, const char *);
    values = snewn(n + 1, const char *);
    i = 0;
    for (val = conf_get_str_strs(conf, CONF_environmt, NULL, &key);
         val != NULL && i < n;
         val = conf_get_str_strs(conf, CONF_environmt, key, &key)) {
        names[i] = key;
        values[i] = val;
        i++;
    }

    vars = snewn(n + KITTY_TERMENV_EXTRA, KittyTermEnvVar);
    list->n = kitty_termenv_merge(names, values, i, kitty_termenv_mode(),
                                  vars);
    list->names = snewn(list->n + 1, char *);
    list->values = snewn(list->n + 1, char *);
    list->added = snewn(list->n + 1, bool);
    list->refused = snewn(list->n + 1, bool);
    list->n_added = 0;
    for (i = 0; i < list->n; i++) {
        list->names[i] = dupstr(vars[i].name);
        list->values[i] = dupstr(vars[i].value);
        list->added[i] = vars[i].added;
        list->refused[i] = false;
        if (vars[i].added)
            list->n_added++;
    }
    sfree(vars);
    sfree(names);
    sfree(values);
    return list;
}

void kitty_termenv_list_free(KittyTermEnvList *list)
{
    size_t i;
    if (!list)
        return;
    for (i = 0; i < list->n; i++) {
        sfree(list->names[i]);
        sfree(list->values[i]);
    }
    sfree(list->names);
    sfree(list->values);
    sfree(list->added);
    sfree(list->refused);
    sfree(list);
}
