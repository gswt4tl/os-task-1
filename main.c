#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* ограничения по размеру для полей file_info */
#define PERM_MAX 11
#define TYPE_MAX 17
#define TIME_MAX 18
#define ID_MAX   30

/* размеры колонок для записи в файл */
unsigned int file_columns[7] = {57, 19, 19, 19, 19, 19, 19};

struct file_info *files = NULL;
int file_counter = 0;
char path[PATH_MAX];
int cursor_pos = 0;
int scroll_pos = 0;
int path_scroll = 0;
int active_column = 0;
int column_scrolls[7];
unsigned int rows;

/* структура для хранения информации
об объекте файловой системы */
struct file_info
{
    char name[NAME_MAX];
    char type[TYPE_MAX];
    char uid[ID_MAX];
    char gid[ID_MAX];
    char permissions[PERM_MAX];
    char mtime[TIME_MAX];
    char atime[TIME_MAX];
    char real_name[NAME_MAX];
};


/* лексикографическая сортировка */
int compare(struct file_info *file_1, struct file_info *file_2)
{
    int is_dir1 = (strcmp(file_1->type, "directory") == 0);
    int is_dir2 = (strcmp(file_2->type, "directory") == 0);

    if (is_dir1 && !is_dir2)  return -1;                  /* если 1 - dir, а 2 - нет => 1 выводим первым */
    if (!is_dir1 && is_dir2)  return 1;                   /* если 2 - dir, а 1 - нет => 2 выводим первым */
    return strcmp(file_1->real_name, file_2->real_name);  /* если оба dir => сравниваем по имени */
}


void sort(struct file_info *files)
{
    if (files == NULL || file_counter == 0)  return;

    for (unsigned int i = 0; i < file_counter; i++)
    {
        for (unsigned int j = 0; j < file_counter - i - 1; j++)
        {
            if (compare(&files[j], &files[j + 1]) > 0)
            {
                struct file_info tmp = files[j];
                files[j] = files[j + 1];
                files[j + 1] = tmp;
            }
        }
    }
}


/* экранирование символов '<' и '>' */
void add_backslash(char *name)
{
    char tmp[NAME_MAX];
    unsigned int j = 0;
    for (unsigned int i = 0; name[i] != 0; i++)
    {
        if (name[i] == '<' || name[i] == '>')
        {
            tmp[j++] = '\\';
            tmp[j++] = name[i];
        }
        else
        {
            tmp[j++] = name[i];
        }
    }
    tmp[j] = 0;
    strcpy(name, tmp);
}


/* получаем имя */
void get_name(struct dirent *rd, char *name)
{
    strcpy(name, rd->d_name);
    add_backslash(name);
}


/* получаем тип объекта файловой системы*/
void get_type(struct stat st, char *type)
{
    switch (st.st_mode & S_IFMT)
    {
        case S_IFBLK:   strcpy(type, "block device");       break;
        case S_IFCHR:   strcpy(type, "character device");   break;
        case S_IFDIR:   strcpy(type, "directory");          break;
        case S_IFIFO:   strcpy(type, "FIFO");               break;
        case S_IFLNK:   strcpy(type, "symlink");            break;
        case S_IFREG:   strcpy(type, "regular file");       break;
        case S_IFSOCK:  strcpy(type, "socket");             break;
        default:        strcpy(type, "unknown");            break;
    }
}

/* получаем имя владельца */
int get_owner(struct stat st, char *uid)
{
    struct passwd *pw = getpwuid(st.st_uid);
    if (pw == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось получить pw.", rows);
        fflush(stdout);
        return -2;
    }
    strcpy(uid, pw->pw_name);
    return 0;
}

/* получаем имя группы */
int get_group(struct stat st, char *gid)
{
    struct group *gr = getgrgid(st.st_gid);
    if (gr == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось получить gr.", rows);
        fflush(stdout);
        return -3;
    }
    strcpy(gid, gr->gr_name);
    return 0;
}

/* получаем права доступа */
void get_permissions(struct stat st, char *permissions)
{
    strcpy(permissions, "----------");

    /* первый символ */
    if (S_ISDIR(st.st_mode))       permissions[0] = 'd';
    else if (S_ISFIFO(st.st_mode)) permissions[0] = 'p';
    else if (S_ISLNK(st.st_mode))  permissions[0] = 'l';
    else if (S_ISBLK(st.st_mode))  permissions[0] = 'b';
    else if (S_ISCHR(st.st_mode))  permissions[0] = 'c';
    else if (S_ISSOCK(st.st_mode)) permissions[0] = 's';

    /* rwxrwxrwx */
    if (st.st_mode & S_IRUSR) permissions[1] = 'r';
    if (st.st_mode & S_IWUSR) permissions[2] = 'w';
    if (st.st_mode & S_IXUSR) permissions[3] = 'x';
    if (st.st_mode & S_IRGRP) permissions[4] = 'r';
    if (st.st_mode & S_IWGRP) permissions[5] = 'w';
    if (st.st_mode & S_IXGRP) permissions[6] = 'x';
    if (st.st_mode & S_IROTH) permissions[7] = 'r';
    if (st.st_mode & S_IWOTH) permissions[8] = 'w';
    if (st.st_mode & S_IXOTH) permissions[9] = 'x';
}

/* получаем дату модификации */
int get_mtime(struct stat st, char *mtime)
{
    struct tm *tmp = localtime(&st.st_mtime);
    if (tmp == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось получить localtime.", rows);
        fflush(stdout);
        return -4;
    }

    /* формат: 01.01.2000 12:30 */
    strftime(mtime, TIME_MAX, "%d.%m.%Y  %H:%M", tmp);
    return 0;
}

/* получаем дату доступа */
int get_atime(struct stat st, char *atime)
{
    struct tm *tmp = localtime(&st.st_atime);
    if (tmp == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось получить localtime.", rows);
        fflush(stdout);
        return -5;
    }

    /* формат: 01.01.2000 12:30 */
    strftime(atime, TIME_MAX, "%d.%m.%Y  %H:%M", tmp);
    return 0;
}

/* получаем список и кол-во объектов в каталоге */
int get_files(char *path, struct file_info **files)
{
    DIR *dir = opendir(path);
    if (dir == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось открыть директорию.", rows);
        fflush(stdout);
        return -6;
    }

    /* если в files уже что-то есть */
    if (*files != NULL) {
        free(*files);
        *files = NULL;
    }

    struct dirent *rd;
    struct file_info *tmp = NULL;
    int local_file_counter = 0;

    while (1)
    {
        rd = readdir(dir);
        if (rd == NULL)
        {
            if (errno != 0)
            {
                wprintf(L"\e[%d;1HНе удалось получить rd.", rows);
                fflush(stdout);
                closedir(dir);
                return -7;
            }
            break;
        }

        /* пропускаем "." и ".." */
        if (strcmp(rd->d_name, ".") == 0 || strcmp(rd->d_name, "..") == 0)
            continue;

        tmp = realloc(*files, (local_file_counter + 1) * sizeof(struct file_info));
        if (tmp == NULL)
        {
            wprintf(L"\e[%d;1HНе удалось выделить память для tmp.", rows);
            fflush(stdout);
            free(*files);
            closedir(dir);
            return -8;
        }
        *files = tmp;

        struct file_info *current_file = *files + local_file_counter;

        /* name */
        get_name(rd, current_file->name);
        strcpy(current_file->real_name, rd->d_name);

        /* stat */
        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", path, rd->d_name);

        struct stat st;
        if (stat(full_path, &st) == -1)
        {
            wprintf(L"\e[%d;1HНе удалось получить stat.", rows);
            fflush(stdout);
            continue;
        }

        /* type */
        get_type(st, current_file->type);

        /* owner */
        if (get_owner(st, current_file->uid) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить имя владельца.", rows);
            fflush(stdout);
            continue;
        }

        /* group */
        if (get_group(st, current_file->gid) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить имя группы.", rows);
            fflush(stdout);
            continue;
        }

        /* permissions */
        get_permissions(st, current_file->permissions);

        /* mtime */
        if (get_mtime(st, current_file->mtime) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить время изменения.", rows);
            fflush(stdout);
            continue;
        }

        /* atime */
        if (get_atime(st, current_file->atime) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить время доступа.", rows);
            fflush(stdout);
            continue;
        }

        local_file_counter++;
    }

    closedir(dir);
    file_counter = local_file_counter;
    sort(*files);
    return local_file_counter;
}


/* расчёт размера колонок */
void count_columns_width(unsigned short x, unsigned int columns[])
{
	/* чтобы таблица выглядела приятнее, делим на 10, а не 9 */
    columns[0] = x * 3/10;
    for (unsigned int i = 1; i < 7; i++)
    {
        columns[i] = x * 1/10;
    }
}


void print_path(char *path, struct winsize ws)
{
    wchar_t saved_path[PATH_MAX];
    mbstowcs(saved_path, path, PATH_MAX);
    saved_path[PATH_MAX - 1] = 0;

    int path_length = wcslen(saved_path);

    /* максимальное количество символов, на которое мы можем промотать */
    int max_scroll = path_length - ws.ws_col;
    if (max_scroll < 0) max_scroll = 0;

    if (path_scroll > max_scroll) path_scroll = max_scroll;
    if (path_scroll < 0)          path_scroll = 0;

    wchar_t displayed_path[PATH_MAX];

    /* весь путь помещается */
    if (path_length <= ws.ws_col)
    {
        wcscpy(displayed_path, saved_path);
    }
    /* в начале - только '>' справа */
    else if (path_scroll == 0)
    {
        wcscpy(displayed_path, saved_path);
        displayed_path[ws.ws_col - 1] = L'>';
        displayed_path[ws.ws_col] = 0;
    }
    /* в конце - только '<' слева */
    else if (path_scroll == max_scroll)
    {
        displayed_path[0] = L'<';
        wcscpy(displayed_path + 1, saved_path + path_scroll + 1);
        displayed_path[ws.ws_col] = 0;
    }
    /* между - и '<', и '>' */
    else
    {
        displayed_path[0] = L'<';
        wcscpy(displayed_path + 1, saved_path + path_scroll + 1);
        displayed_path[ws.ws_col - 1] = L'>';
        displayed_path[ws.ws_col] = 0;
    }

    wprintf(L"\e[1;38;5;200m%-*ls\e[0m\n", ws.ws_col, displayed_path);
}


void print_string(char *str, unsigned int column_width, unsigned int column_index) {
    if (column_width <= 0) return;

    wchar_t displayed_string[NAME_MAX];
    mbstowcs(displayed_string, str, NAME_MAX);
    displayed_string[NAME_MAX - 1] = 0;

    int string_length = wcslen(displayed_string);

    /* если строка полностью помещается */
    if (string_length <= column_width)
    {
        wprintf(L"%-*ls", column_width, displayed_string);
        return;
    }

    int s = column_scrolls[column_index];
    if (s < 0) s = 0;
    if (s > string_length - column_width) s = string_length - column_width;
    column_scrolls[column_index] = s;

    wchar_t buf[NAME_MAX];

    /* заполним буфер пробелами */
    for (int i = 0; i < column_width; i++) buf[i] = L' ';
    buf[column_width] = 0;

    /* копируем видимую часть строки */
    wcsncpy(buf, displayed_string + s, column_width);

    if (s > 0) buf[0] = L'<';
    if (s + column_width < string_length) buf[column_width - 1] = L'>';

    wprintf(L"%-*ls", column_width, buf);
}


void display_data(struct file_info file, unsigned int columns[], struct winsize ws)
{
    char *file_ptr;
    for (unsigned int i = 0; i < 7; i++)
    {
        switch (i)
        {
            case 0: file_ptr = file.name;         break;
            case 1: file_ptr = file.type;         break;
            case 2: file_ptr = file.uid;          break;
            case 3: file_ptr = file.gid;          break;
            case 4: file_ptr = file.permissions;  break;
            case 5: file_ptr = file.mtime;        break;
            case 6: file_ptr = file.atime;        break;
        }

        print_string(file_ptr, columns[i], i);

        if (i < 6) wprintf(L"|");
    }
    putwchar(L'\n');
}


int display_in_terminal(char *path)
{
    /* очищаем экран и перемещаем курсор на начало экрана */
    wprintf(L"\e[2J\e[H");

    /* получаем размер окна */
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1)  /* stdout = 1 */
    {
        wprintf(L"\e[%d;1HНе удалось получить размер окна терминала.", rows);
        fflush(stdout);
        return -9;
    }
    rows = ws.ws_row;

    /* считаем столбцы для таблицы */
	unsigned int columns[7];
    count_columns_width(ws.ws_col, columns);

    /* считаем строки для таблицы, чтобы заголовки остались сверху */
    int height = ws.ws_row - 3;
    if (height < 1)
    {
        height = 1;
    }

    /* вывод заголовков */
    print_path(path, ws);

    for (int i = 0; i < 7; i++)
    {
        if (i == active_column)
        {
            wprintf(L"\e[1;3;48;5;198m");
        }
        else
        {
            wprintf(L"\e[3;38;5;198m");
        }

        char *column_name;
        switch (i)
        {
            case 0: column_name = "name";         break;
            case 1: column_name = "type";         break;
            case 2: column_name = "owner";        break;
            case 3: column_name = "group";        break;
            case 4: column_name = "permissions";  break;
            case 5: column_name = "mtime";        break;
            case 6: column_name = "atime";        break;
        }

        print_string(column_name, columns[i], i);

        wprintf(L"\e[0m");
        if (i < 6) wprintf(L"|");
    }
    putwchar(L'\n');

    /* вывод таблицы */
    int data_end = height + scroll_pos;
    if (data_end > file_counter)
    {
        data_end = file_counter;
    }

    for (int i = scroll_pos; i < data_end; i++)
    {
        if (i == cursor_pos)
        {
            wprintf(L"\e[1;48;5;212m");
        }
        display_data(files[i], columns, ws);
        if (i == cursor_pos)
        {
            wprintf(L"\e[0m");
        }
    }

    return 0;
}


void winsize_changed(int signum)
{
    display_in_terminal(path);
}


int keyboard_input()
{
    char input;
    int read_bytes = read(0, &input, 1);  /* stdin = 0 */

    if (read_bytes == 1)
    {
        switch (input)
        {
            default:  return 0;
            case 'q': return -1; /* выход из программы */
            case 'Q': return -1;
            case 4:   return -1; /* 4 - ctrl + d */

            /* промотка пути */
            case '<':
                path_scroll--;
                if (path_scroll < 0)
                {
                    path_scroll = 0;
                }
                return 1;

            case '>':
                path_scroll++;
                return 1;

            /* переключение активного столбца */
            case '[':
                if (active_column > 0)
                {
                    active_column--;
                    return 1;
                }
                break;

            case ']':
                if (active_column < 6)
                {
                    active_column++;
                    return 1;
                }
                break;

            /* переход на родительский каталог */
            case '^':
                if (chdir("..") == 0)
                {
                    if (getcwd(path, PATH_MAX) == NULL)
                    {
                        wprintf(L"\e[%d;1HНе удалось получить путь к рабочему каталогу.", rows);
                        fflush(stdout);
                        return 0;
                    }

                    free(files);
                    files = NULL;

                    file_counter = get_files(path, &files);
                    if (file_counter < 0)
                    {
                        file_counter = 0;
                        wprintf(L"\e[%d;1HНе удалось получить файлы в директории.", rows);
                        fflush(stdout);
                        return 0;
                    }

                    cursor_pos = 0;
                    active_column = 0;
                    scroll_pos = 0;
                    path_scroll = 0;
                    for (int i = 0; i < 7; i++)
                    {
                        column_scrolls[i] = 0;
                    }

                    return 1;
                }
                break;

            /* переход в выбранный каталог */
            case '\n':
                if (strcmp(files[cursor_pos].type, "directory") == 0)
                {
                    char full_path[PATH_MAX];
                    snprintf(full_path, PATH_MAX, "%s/%s", path, files[cursor_pos].real_name);

                    if (chdir(full_path) == 0)
                    {
                        if (getcwd(path, PATH_MAX) == NULL)
                        {
                            wprintf(L"\e[%d;1HНе удалось получить путь к рабочему каталогу.", rows);
                            fflush(stdout);
                            return 0;
                        }

                        free(files);
                        files = NULL;

                        file_counter = get_files(path, &files);
                        if (file_counter < 0)
                        {
                            file_counter = 0;
                            wprintf(L"\e[%d;1HНе удалось получить файлы в директории.", rows);
                            fflush(stdout);
                            return 0;
                        }

                        cursor_pos = 0;
                        active_column = 0;
                        scroll_pos = 0;
                        path_scroll = 0;
                        for (int i = 0; i < 7; i++)
                        {
                            column_scrolls[i] = 0;
                        }

                        return 1;
                    }
                    else
                    {
                        wprintf(L"\e[%d;1HНет доступа к директории.", rows);
                        fflush(stdout);
                        return 0;
                    }
                }
                break;

            case 27:  /* 27 - стрелки */
                char esc[2];
                if (read(0, &esc[0], 1) == 1 && read(0, &esc[1], 1) == 1)
                {
                    if (esc[0] == '[')
                    {
                        switch (esc[1])
                        {
                            default:  return 0;
                            case 'A':  /* стрелка вверх */
                                if (cursor_pos > 0)
                                {
                                    cursor_pos--;
                                    if (cursor_pos < scroll_pos)
                                    {
                                        scroll_pos = cursor_pos;
                                    }
                                    return 1;  /* перерисовка */
                                }
                                break;

                            case 'B':  /* стрелка вниз */
                                if (cursor_pos < (file_counter - 1))
                                {
                                    cursor_pos++;

                                    struct winsize ws;
                                    rows = ws.ws_row;
                                    if (ioctl(1, TIOCGWINSZ, &ws) != -1)
                                    {
                                        int height = ws.ws_row - 3;
                                        if (height < 1)
                                        {
                                            height = 1;
                                        }
                                        if (cursor_pos >= scroll_pos + height)
                                        {
                                            scroll_pos = cursor_pos - height + 1;
                                        }
                                    }
                                    return 1;
                                }
                                break;

                            /* промотка активного столбца */
                            case 'C':  /* стрелка вправо */
                                column_scrolls[active_column]++;
                                return 1;

                            case 'D':  /* стрелка влево */
                                column_scrolls[active_column]--;
                                if (column_scrolls[active_column] < 0)
                                    column_scrolls[active_column] = 0;
                                return 1;
                        }
                    }
                }
        }
    }

    return 0;
}


void display_data_in_file(struct file_info file, unsigned int columns[])
{
    char *file_ptr;
    wchar_t wc_file_ptr[NAME_MAX];
    for (unsigned int i = 0; i < 7; i++)
    {
        switch (i)
        {
            case 0: file_ptr = file.name;         break;
            case 1: file_ptr = file.type;         break;
            case 2: file_ptr = file.uid;          break;
            case 3: file_ptr = file.gid;          break;
            case 4: file_ptr = file.permissions;  break;
            case 5: file_ptr = file.mtime;        break;
            case 6: file_ptr = file.atime;        break;
        }

        mbstowcs(wc_file_ptr, file_ptr, NAME_MAX);
        wprintf(L"%-*ls", columns[i], wc_file_ptr);

        if (i < 6) wprintf(L"|");
    }
    putwchar(L'\n');
}


/* рекурсивная функция для вывода в файл */
void display_files_recursive(char *current_path, unsigned int columns[])
{
    struct file_info *local_files = NULL;
    int local_file_count = 0;

    DIR *dir = opendir(current_path);
    if (dir == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось открыть директорию.", rows);
        fflush(stdout);
        return;
    }

    struct dirent *rd;
    while (1)
    {
        rd = readdir(dir);
        if (rd == NULL)
        {
            if (errno != 0)
            {
                wprintf(L"\e[%d;1HНе удалось получить rd.", rows);
                fflush(stdout);
                closedir(dir);
                return;
            }
            break;
        }

        /* пропускаем "." и ".." */
        if (strcmp(rd->d_name, ".") == 0 || strcmp(rd->d_name, "..") == 0)
        {
            continue;
        }

        struct file_info *tmp = realloc(local_files, (local_file_count + 1) * sizeof(struct file_info));
        if (tmp == NULL)
        {
            wprintf(L"\e[%d;1HНе удалось выделить память для tmp.", rows);
            fflush(stdout);
            closedir(dir);
            free(local_files);
            return;
        }
        local_files = tmp;

        struct file_info *current_file = local_files + local_file_count;

        get_name(rd, current_file->name);
        strcpy(current_file->real_name, rd->d_name);

        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", current_path, rd->d_name);

        struct stat st;
        if (stat(full_path, &st) == -1)
        {
            wprintf(L"\e[%d;1HНе удалось получить stat.", rows);
            fflush(stdout);
            continue;
        }

        get_type(st, current_file->type);

        if (get_owner(st, current_file->uid) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить имя владельца.", rows);
            fflush(stdout);
            continue;
        }

        if (get_group(st, current_file->gid) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить имя группы.", rows);
            fflush(stdout);
            continue;
        }

        get_permissions(st, current_file->permissions);

        if (get_mtime(st, current_file->mtime) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить время изменения.", rows);
            fflush(stdout);
            continue;
        }

        if (get_atime(st, current_file->atime) != 0)
        {
            wprintf(L"\e[%d;1HНе удалось получить время доступа.", rows);
            fflush(stdout);
            continue;
        }

        local_file_count++;
    }
    closedir(dir);

    /* сортируем файлы в текущей директории */
    if (local_files != NULL && local_file_count > 0)
    {
        for (unsigned int i = 0; i < local_file_count; i++)
        {
            for (unsigned int j = 0; j < local_file_count - i - 1; j++)
            {
                if (compare(&local_files[j], &local_files[j + 1]) > 0)
                {
                    struct file_info tmp = local_files[j];
                    local_files[j] = local_files[j + 1];
                    local_files[j + 1] = tmp;
                }
            }
        }
    }

    for (int i = 0; i < local_file_count; i++)
    {
        if (strcmp(local_files[i].type, "directory") != 0)
        {
            display_data_in_file(local_files[i], columns);
        }
    }

    /* рекурсивно обрабатываем подкаталоги */
    for (int i = 0; i < local_file_count; i++)
    {
        if (strcmp(local_files[i].type, "directory") == 0)
        {
            char subdir_path[PATH_MAX];
            snprintf(subdir_path, PATH_MAX, "%s/%s", current_path, local_files[i].real_name);
            display_files_recursive(subdir_path, columns);
        }
    }

    free(local_files);
}


int main()
{
    setlocale(LC_ALL, "");

    /* устанавливаем обработчик сигнала SIGWINCH */
    struct sigaction sigact;
    sigact.sa_handler = winsize_changed;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART;
    if (sigaction(SIGWINCH, &sigact, NULL) == -1)
    {
        wprintf(L"\e[%d;1HНе удалось выставить обработчик сигнала SIGWINCH.", rows);
        fflush(stdout);
        return -10;
    }

    /* находим путь к рабочей директории */
    if (getcwd(path, PATH_MAX) == NULL)
    {
        wprintf(L"\e[%d;1HНе удалось получить путь рабочей директории.", rows);
        fflush(stdout);
        return -11;
    }

    /* считаем кол-во файлов в директории */
    file_counter = get_files(path, &files);
    if (file_counter < 0)
    {
        wprintf(L"\e[%d;1HНе удалось вернуть настройки терминала.", rows);
        fflush(stdout);
        return -12;
    }

    /* если записываем в файл */
	if (isatty(1) == 0)
    {
        wchar_t wc_path[PATH_MAX];
        mbstowcs(wc_path, path, PATH_MAX);
        wprintf(L"%ls\n", wc_path);
        wprintf(L"%-*ls|%-*ls|%-*ls|%-*ls|%-*ls|%-*ls|%-*ls\n",
            file_columns[0], L"name", file_columns[1], L"type", file_columns[2], L"owner",
            file_columns[3], L"group", file_columns[4], L"permissions",
            file_columns[5], L"mtime", file_columns[6], L"atime");

        display_files_recursive(path, file_columns);

        free(files);
		return 0;
    }

    /* если выводим в терминал */

    /* переводим терминал в режим обработки ввода */
    struct termios old, new;
    if (tcgetattr(0, &old) == -1)
    {
        wprintf(L"\e[%d;1HНе удалось получить настройки терминала.", rows);
        fflush(stdout);
        return -13;
    }
    memcpy(&new, &old, sizeof(struct termios));
    /* инвертируем биты, затем применяем маску к c_lflag через побитовое И */
    new.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(0, TCSANOW, &new) == -1)
    {
        wprintf(L"\e[%d;1HНе удалось вернуть настройки терминала.", rows);
        fflush(stdout);
        free(files);
        return -14;
    }

    /* первоначальное отображение */
    if (display_in_terminal(path) != 0)
    {
        if (tcsetattr(0, TCSANOW, &old) == -1)
        {
            wprintf(L"\e[%d;1HНе удалось вернуть настройки терминала.", rows);
            fflush(stdout);
            free(files);
            return -15;
        }
        wprintf(L"\e[%d;1HНе удалось вывести данные в терминал.", rows);
        fflush(stdout);
        free(files);
        return -16;
    }

    while (1)
    {
        /* обработка ввода в терминал */
        int input_result = keyboard_input();
        if (input_result == -1)  break;
        else if (input_result == 1)
        {
            if (display_in_terminal(path) != 0)
            {
                if (tcsetattr(0, TCSANOW, &old) == -1)
                {
                    wprintf(L"\e[%d;1HНе удалось вернуть настройки терминала.", rows);
                    fflush(stdout);
                    free(files);
                    return -17;
                }
                wprintf(L"\e[%d;1HНе удалось вывести данные в терминал.", rows);
                fflush(stdout);
                free(files);
                return -18;
            }
        }
    }

    wprintf(L"\e[2J\e[H");
    if (tcsetattr(0, TCSANOW, &old) == -1)
    {
        wprintf(L"\e[%d;1HНе удалось вернуть настройки терминала.", rows);
        fflush(stdout);
        free(files);
        return -19;
    }

    free(files);
    return 0;
}
