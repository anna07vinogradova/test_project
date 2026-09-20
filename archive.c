#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"
#include "huffman.h"
#include "bitio.h"
#include "crc32.h"

#define HARC_VERSION 1

const char HARC_MAGIC[4] = {'H', 'A', 'R', 'C'};

typedef struct archive_entry {
    char * name;    // имя файла
    unsigned int original_size;     // исходный размер
    int symbols_count;      // колво символов
    unsigned int data_size;
    unsigned int crc;
} archive_entry;

static const char * get_base_name(const char * path) {  // извлекаем имя файла из полного пути
    const char * last = path;

    for (const char * p = path; *p != '\0'; p++) {
        if (*p == '\\') {
            last = p + 1;
        }
    }

    return last;
}

static char * make_temp_name(const char * archive_name) {   // создаём имя временного архива
    int len = strlen(archive_name);
    char * temp = (char*)calloc(len + 6, sizeof(char));

    if (temp == NULL) {
        return NULL;
    }

    strcpy(temp, archive_name);
    strcat(temp, ".tmp");
    return temp;
}

// далее write/read_u16/32 записывают и считывают числа в little endian 16/32 бита соответственно
static int write_u16(FILE * fout, unsigned int x) {
    return fputc(x & 255, fout) != EOF && fputc((x >> 8) & 255, fout) != EOF;
}

static int write_u32(FILE * fout, unsigned int x) {
    return fputc(x & 255, fout) != EOF &&
           fputc((x >> 8) & 255, fout) != EOF &&
           fputc((x >> 16) & 255, fout) != EOF &&
           fputc((x >> 24) & 255, fout) != EOF;
}

static int read_u16(FILE * fin, unsigned int * x) {
    int b0 = fgetc(fin);
    int b1 = fgetc(fin);

    if (b0 == EOF || b1 == EOF) {
        return 0;
    }

    *x = (unsigned int)b0 | ((unsigned int)b1 << 8);
    return 1;
}

static int read_u32(FILE * fin, unsigned int * x) {
    int b0 = fgetc(fin);
    int b1 = fgetc(fin);
    int b2 = fgetc(fin);
    int b3 = fgetc(fin);

    if (b0 == EOF || b1 == EOF || b2 == EOF || b3 == EOF) {
        return 0;
    }

    *x = (unsigned int)b0 |
         ((unsigned int)b1 << 8) |
         ((unsigned int)b2 << 16) |
         ((unsigned int)b3 << 24);
    return 1;
}

// записываем заголовок файла:
// "HARC"
// версия
// кол-во файлов
static int write_archive_header(FILE * fout, unsigned int files_count) {
    if (fwrite(HARC_MAGIC, 1, 4, fout) != 4) {
        return 0;
    }

    if (fputc(HARC_VERSION, fout) == EOF) {
        return 0;
    }

    if (!write_u32(fout, files_count)) {
        return 0;
    }

    return 1;
}

// исправляем количество файлов в уже записанном заголовке
// нужно т.к при создании временного архива не всегда известно итоговое количество файлов
// поэтому в количество файлов изначально пишется 0 а потом изменяется
static int patch_archive_count(FILE * fout, unsigned int files_count) {
    if (fseek(fout, 5, SEEK_SET) != 0) {
        return 0;
    }

    if (!write_u32(fout, files_count)) {
        return 0;
    }

    if (fseek(fout, 0, SEEK_END) != 0) {
        return 0;
    }

    return 1;
}

// считываем заголовок файла и проверяем корректность
static int read_archive_header(FILE * fin, unsigned int * files_count) {
    char magic[4];

    if (fread(magic, 1, 4, fin) != 4) {
        return 0;
    }

    if (memcmp(magic, HARC_MAGIC, 4) != 0) {
        return 0;
    }

    int version = fgetc(fin);

    if (version != HARC_VERSION) {
        return 0;
    }

    if (!read_u32(fin, files_count)) {
        return 0;
    }

    return 1;
}

// записываем заголовок для одного файла
static int write_entry_header(FILE * fout, archive_entry * e) {
    unsigned int name_len = strlen(e->name);

    if (name_len == 0 || name_len > 65535) {    // 65535 - 2 байта
        return 0;
    }

    if (!write_u16(fout, name_len)) {
        return 0;
    }

    if (fwrite(e->name, 1, name_len, fout) != name_len) {
        return 0;
    }

    if (!write_u32(fout, e->original_size)) {
        return 0;
    }

    if (fputc(e->symbols_count % 256, fout) == EOF) {
        return 0;
    }

    if (!write_u32(fout, e->data_size)) {
        return 0;
    }

    if (!write_u32(fout, e->crc)) {
        return 0;
    }

    return 1;
}

// читаем заголовок одного файла
static int read_entry_header(FILE * fin, archive_entry * e) {
    unsigned int name_len;

    if (!read_u16(fin, &name_len)) {
        return 0;
    }

    if (name_len == 0 || name_len > 65535) {
        return 0;
    }

    e->name = (char*)calloc(name_len + 1, sizeof(char));

    if (e->name == NULL) {
        return 0;
    }

    if (fread(e->name, 1, name_len, fin) != name_len) {
        free(e->name);
        e->name = NULL;
        return 0;
    }

    if (!read_u32(fin, &e->original_size)) {
        free(e->name);
        e->name = NULL;
        return 0;
    }

    int n = fgetc(fin);

    if (n == EOF) {
        free(e->name);
        e->name = NULL;
        return 0;
    }

    if (n == 0) {   // в 1 байте максимум число 255 
        e->symbols_count = 256;
    }
    else {
        e->symbols_count = n;
    }

    if (!read_u32(fin, &e->data_size)) {
        free(e->name);
        e->name = NULL;
        return 0;
    }

    if (!read_u32(fin, &e->crc)) {
        free(e->name);
        e->name = NULL;
        return 0;
    }

    return 1;
}

static void free_entry(archive_entry * e) {
    if (e->name != NULL) {
        free(e->name);
        e->name = NULL;
    }
}

// копируем байты в новый временный архив, используется при добавлении или удалении файлов
static int copy_bytes(FILE * fin, FILE * fout, unsigned int count) {
    unsigned char buffer[4096];

    while (count > 0) {
        unsigned int part = count;

        if (part > sizeof(buffer)) {
            part = sizeof(buffer);
        }

        if (fread(buffer, 1, part, fin) != part) {
            return 0;
        }

        if (fwrite(buffer, 1, part, fout) != part) {
            return 0;
        }

        count -= part;
    }

    return 1;
}

// пропускаем count байтов, например при -l нужно прочитать только заголовки, а дерево и закодированные
// файлы пропускаем
static int skip_bytes(FILE * fin, unsigned int count) {
    return fseek(fin, count, SEEK_CUR) == 0;
}

// сколько байтов занимает записанное в архив дерево Хафмана
static unsigned int get_tree_bytes(int symbols_count) {
    if (symbols_count <= 0) {
        return 0;
    }

    // листья = 9 бит * n | внутренние вершины = n - 1 бит
    unsigned int tree_bits = 9 * (unsigned int)symbols_count + (unsigned int)symbols_count - 1;
    return (tree_bits + 7) / 8;     // возвращаем байты
}

// проверка есть ли файл с таким именем в списке аргументов
static int name_in_list(const char * name, int files_count, char ** files) {
    for (int i = 0; i < files_count; i++) {
        if (strcmp(name, get_base_name(files[i])) == 0) {
            return 1;
        }
    }

    return 0;
}

// возвращает индекс файла в списке аргументов
static int name_index_in_list(const char * name, int files_count, char ** files) {
    for (int i = 0; i < files_count; i++) {
        if (strcmp(name, get_base_name(files[i])) == 0) {
            return i;
        }
    }

    return -1;
}

// считываем файл для дальнейшего использования
static int read_file_to_memory(const char * file_name, unsigned char ** text, int * len) {
    FILE * fin = fopen(file_name, "rb");

    if (fin == NULL) {
        printf("Ошибка: Не удалось открыть файл '%s'.\n", file_name);
        return 0;
    }

    int capacity = 1024;
    *text = (unsigned char*)malloc(capacity * sizeof(unsigned char));   // исходная строка символов

    if (*text == NULL) {
        fclose(fin);
        return 0;
    }

    *len = 0;
    int c;

    while ((c = fgetc(fin)) != EOF) {
        if (*len == capacity) {
            capacity *= 2;
            unsigned char * temp = (unsigned char*)realloc(*text, capacity * sizeof(unsigned char));

            if (temp == NULL) {
                free(*text);
                *text = NULL;
                fclose(fin);
                return 0;
            }

            *text = temp;
        }

        (*text)[(*len)++] = (unsigned char)c;
    }

    fclose(fin);
    return 1;
}

// сжимаем один файл
static int pack_one_file(FILE * fout, const char * file_name) {
    unsigned char * text = NULL;
    int len = 0;

    if (!read_file_to_memory(file_name, &text, &len)) {
        return 0;
    }

    const char * base = get_base_name(file_name);	// получаем имя файла

    if (base[0] == '\0') {
        printf("Ошибка: пустое имя файла '%s'.\n", file_name);
        free(text);
        return 0;
    }

    int freq[256];	// частоты символов
    int n = 0;
    huffman * tree = build_huffman_tree(text, len, &n, freq);

    char ** code_word = (char**)malloc(sizeof(char*) * 256);    // кодовые слова для символов

    if (code_word == NULL) {
        free(text);
        free_tree(tree);
        return 0;
    }

    for (int i = 0; i < 256; i++) {
        code_word[i] = (char*)calloc(300, sizeof(char));

        if (code_word[i] == NULL) {
            for (int j = 0; j < i; j++) {
                free(code_word[j]);
            }
            free(code_word);
            free(text);
            free_tree(tree);
            return 0;
        }
    }

    if (tree != NULL) {
        char path[300];
        build_codes(tree, path, 0, code_word);       // записываем кодовые слова для символов
    }

    long long data_bits = 0;	// размер закодированных данных

    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            data_bits += (long long)freq[i] * (long long)strlen(code_word[i]);
        }
    }

    archive_entry e;
    e.name = (char*)base;
    e.original_size = (unsigned int)len;
    e.symbols_count = n;
    e.data_size = (unsigned int)((data_bits + 7) / 8);
    e.crc = crc32_bytes(text, len);

    if (!write_entry_header(fout, &e)) {
        printf("Ошибка: не получилось записать заголовок файла '%s'.\n", file_name);
        for (int i = 0; i < 256; i++) {
            free(code_word[i]);
        }
        free(code_word);
        free(text);
        free_tree(tree);
        return 0;
    }

    byte b;
    b.buffer = 0;
    b.bits_count = 0;

    if (tree != NULL) {
        prefix(tree, &b, fout);      // обход дерева Хафммана
        add_bits(&b, fout);
    }

    // записываем закодированную последовательность
    for (int i = 0; i < len; i++) {
        char * s = code_word[text[i]];

        for (int j = 0; s[j] != '\0'; j++) {
            write_bit(&b, s[j] - '0', fout);
        }
    }

    add_bits(&b, fout);

    for (int i = 0; i < 256; i++) {
        free(code_word[i]);
    }

    free(code_word);
    free_tree(tree);
    free(text);

    return 1;
}

// декодируем один файл
static int decode_current_entry(FILE * fin, archive_entry * e, FILE * fout, unsigned int * real_crc) {
    long tree_start = ftell(fin);	// ставим указатель начиная с дерева Хаффмана

    if (tree_start < 0) {
        return 0;
    }

	// если файл пуст
    if (e->original_size == 0 && e->data_size == 0) {
        *real_crc = crc32_bytes(NULL, 0);
        return 1;
    }

    byte b;
    b.buffer = 0;
    b.bits_count = 0;

    int ok = 1;
    huffman * wood = read_tree(&b, fin, &ok);     // готово дерево Хафммана

    if (ok == 0 || wood == NULL) {
        free_tree(wood);
        return 0;
    }

    long data_start = tree_start + get_tree_bytes(e->symbols_count);	// перемещаемя на начало сжатых данных

    if (fseek(fin, data_start, SEEK_SET) != 0) {
        free_tree(wood);
        return 0;
    }

    b.buffer = 0;
    b.bits_count = 0;

    unsigned int crc = 0xFFFFFFFFU;

    // если в файле был только один уникальный символ
    if (wood->left == NULL && wood->right == NULL) {
        for (unsigned int i = 0; i < e->original_size; i++) {
            unsigned char symbol = (unsigned char)wood->ascii;

            if (fout != NULL) {
                fputc(symbol, fout);
            }

            crc = crc32_update(crc, symbol);
        }
    }
    else {
        // декодируем закодированную последовательность
        for (unsigned int i = 0; i < e->original_size; i++) {
            huffman * p = wood;

            while (p->left != NULL || p->right != NULL) {
                int bit = read_bit(&b, fin);

                if (bit == -1) {	// архив поврежден
                    free_tree(wood);
                    return 0;
                }

                if (bit == 0) {
                    p = p->left;
                }
                else {
                    p = p->right;
                }

                if (p == NULL) {
                    free_tree(wood);
                    return 0;
                }
            }

            unsigned char symbol = (unsigned char)p->ascii;

            if (fout != NULL) {
                fputc(symbol, fout);
            }

            crc = crc32_update(crc, symbol);
        }
    }

    crc ^= 0xFFFFFFFFU;
    *real_crc = crc;

    free_tree(wood);

    if (fseek(fin, data_start + e->data_size, SEEK_SET) != 0) {		// переходим к следующей записи архива
        return 0;
    }

    return 1;
}

// операция добавления файлов в архив -a
int archive_files(const char * archive_name, int files_count, char ** files) {
    if (files_count <= 0) {
        printf("Ошибка: не указаны файлы.\n");
        return 1;
    }

    char * temp_name = make_temp_name(archive_name);

    if (temp_name == NULL) {
        return 1;
    }

    FILE * old = fopen(archive_name, "rb");
    FILE * temp = fopen(temp_name, "wb");

    if (temp == NULL) {
        printf("Ошибка: не удалось открыть временный архив.\n");
        if (old != NULL) {
            fclose(old);
        }
        free(temp_name);
        return 1;
    }

    if (!write_archive_header(temp, 0)) {
        printf("Ошибка: не удалось записать заголовок архива.\n");
        if (old != NULL) {
            fclose(old);
        }
        fclose(temp);
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    unsigned int result_count = 0;	// кол-во файлов в итоговом архиве

    if (old != NULL) {
        unsigned int old_count;		// кол-во файлов в старом архиве

        if (!read_archive_header(old, &old_count)) {
            printf("Ошибка: не удалось прочитать заголовок старого архива\n");
            fclose(old);
            fclose(temp);
            remove(temp_name);
            free(temp_name);
            return 1;
        }

		// блок проверки на наличие файлов с одинаковым именем, если они есть то
		// их не берём в временный архив как бы удаляя
        for (unsigned int i = 0; i < old_count; i++) {
            archive_entry e;
            e.name = NULL;

            if (!read_entry_header(old, &e)) {
                printf("Ошибка: не удалось прочитать заголовок у одного из файлов старого архива.\n");
                fclose(old);
                fclose(temp);
                remove(temp_name);
                free(temp_name);
                return 1;
            }

			// размер дерева хаффмана + размер закодированных данных
            unsigned int payload_size = get_tree_bytes(e.symbols_count) + e.data_size;

			// проверка на наличие файла, если он уже есть, то просто пропускаем его как бы удаляя
            // например при добавлении файла, который уже есть
            if (name_in_list(e.name, files_count, files)) {
                if (!skip_bytes(old, payload_size)) {
                    printf("Ошибка: выход за границы файла при пропуске закодированной информации.\n");
                    free_entry(&e);
                    fclose(old);
                    fclose(temp);
                    remove(temp_name);
                    free(temp_name);
                    return 1;
                }
            }
			// если файла нет, то сохраняем его
            else {
                if (!write_entry_header(temp, &e) || !copy_bytes(old, temp, payload_size)) {
                    printf("Ошибка: не удалось скопировать файл из старого архива в временный.\n");
                    free_entry(&e);
                    fclose(old);
                    fclose(temp);
                    remove(temp_name);
                    free(temp_name);
                    return 1;
                }

                result_count++;
            }

            free_entry(&e);
        }

        fclose(old);
    }

	// добавляем новые файлы
    for (int i = 0; i < files_count; i++) {
        if (!pack_one_file(temp, files[i])) {
            fclose(temp);
            remove(temp_name);
            free(temp_name);
            return 1;
        }

        result_count++;
    }

	// правим количесто файлов в архиве
    if (!patch_archive_count(temp, result_count)) {
        printf("Ошибка: не удалось сосчитать количество файлов в архиве\n");
        fclose(temp);
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    fclose(temp);

	// удаляем исходный архив
    remove(archive_name);

	// переименовываем временный архив
    if (rename(temp_name, archive_name) != 0) {
        printf("Ошибка: не удалось переименовать временный архив\n");
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    printf("Файлов в архиве: %u\n", result_count);
    free(temp_name);
    return 0;
}

// операция извлечения файлов из архива -x
int extract_files(const char * archive_name, int files_count, char ** files) {
    FILE * fin = fopen(archive_name, "rb");

    if (fin == NULL) {
        printf("Ошибка: не удалось открыть архив '%s'.\n", archive_name);
        return 1;
    }

    unsigned int entries_count;

    if (!read_archive_header(fin, &entries_count)) {
        printf("Ошибка: не удалось прочитать заголовок архива\n");
        fclose(fin);
        return 1;
    }

    int * found = NULL;

    if (files_count > 0) {
        found = (int*)calloc(files_count, sizeof(int));
    }

    for (unsigned int i = 0; i < entries_count; i++) {
        archive_entry e;
        e.name = NULL;

        if (!read_entry_header(fin, &e)) {
            printf("Ошибка: не удалось прочитать заголовок файла\n");
            free(found);
            fclose(fin);
            return 1;
        }

		// индекс файла в архиве
        int index = name_index_in_list(e.name, files_count, files);
		// если извлекаем все файлы архива, то files_count = 0, если конкретно этот файл то index != -1
        int need_extract = (files_count == 0 || index != -1);

        if (need_extract) {
            FILE * fout = fopen(e.name, "wb");

            if (fout == NULL) {
                printf("Ошибка: не удалось создать файл '%s'.\n", e.name);
                free_entry(&e);
                free(found);
                fclose(fin);
                return 1;
            }

            unsigned int real_crc = 0;

			// декодируем файл
            if (!decode_current_entry(fin, &e, fout, &real_crc)) {
                printf("Ошибка: не удалось извлечь файл '%s'. Архив повреждён.\n", e.name);
                fclose(fout);
                free_entry(&e);
                free(found);
                fclose(fin);
                return 1;
            }

            fclose(fout);

            if (real_crc != e.crc) {
                printf("Ошибка: различие crc - файл поврежден '%s'.\n", e.name);
                free_entry(&e);
                free(found);
                fclose(fin);
                return 1;
            }

            if (index != -1) {
                found[index] = 1;
            }

            printf("Успешно извлечён: %s\n", e.name);
        }

		// файл не нужен, пропускаем его
        else {
            if (!skip_bytes(fin, get_tree_bytes(e.symbols_count) + e.data_size)) {
                printf("Ошибка: выход за границы файла при декодировании архива.\n");
                free_entry(&e);
                free(found);
                fclose(fin);
                return 1;
            }
        }

        free_entry(&e);
    }

	// проверка все ли файлы найдены в архиве
    for (int i = 0; i < files_count; i++) {
        if (found[i] == 0) {
            printf("Внимание: файл '%s' не найден в архиве\n", get_base_name(files[i]));
        }
    }

    free(found);
    fclose(fin);
    return 0;
}

// операция удаления файлов -d
int delete_files(const char * archive_name, int files_count, char ** files) {
    if (files_count <= 0) {
        printf("Ошибка: не указаны файлы для удаления\n");
        return 1;
    }

    FILE * old = fopen(archive_name, "rb");

    if (old == NULL) {
        printf("Ошибка: не удалось открыть архив '%s'.\n", archive_name);
        return 1;
    }

    unsigned int old_count;

    if (!read_archive_header(old, &old_count)) {
        printf("Ошибка: не удалось прочитать заголовок архива при удалении файлов.\n");
        fclose(old);
        return 1;
    }

    char * temp_name = make_temp_name(archive_name);

    if (temp_name == NULL) {
        fclose(old);
        return 1;
    }

    FILE * temp = fopen(temp_name, "wb");

    if (temp == NULL) {
        printf("Ошибка: не удалось создать временный архив\n");
        fclose(old);
        free(temp_name);
        return 1;
    }

    if (!write_archive_header(temp, 0)) {
        fclose(old);
        fclose(temp);
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    int * found = (int*)calloc(files_count, sizeof(int));
    unsigned int result_count = 0;

    for (unsigned int i = 0; i < old_count; i++) {
        archive_entry e;
        e.name = NULL;

        if (!read_entry_header(old, &e)) {
            printf("Ошибка: не удалось прочитать заголовок файла при удалении файлов\n");
            free(found);
            fclose(old);
            fclose(temp);
            remove(temp_name);
            free(temp_name);
            return 1;
        }

        unsigned int payload_size = get_tree_bytes(e.symbols_count) + e.data_size;
        int index = name_index_in_list(e.name, files_count, files);

        if (index != -1) {
            found[index] = 1;
            printf("Удалён из архива: %s\n", e.name);

            if (!skip_bytes(old, payload_size)) {
                printf("Ошибка: выход за границы файла при удалении файлов\n");
                free_entry(&e);
                free(found);
                fclose(old);
                fclose(temp);
                remove(temp_name);
                free(temp_name);
                return 1;
            }
        }

		// если файл не нужно удалять то сохраняем его в временный архив
        else {
            if (!write_entry_header(temp, &e) || !copy_bytes(old, temp, payload_size)) {
                printf("Ошибка: не удалось сохранить файл в временный архив при удалении");
                free_entry(&e);
                free(found);
                fclose(old);
                fclose(temp);
                remove(temp_name);
                free(temp_name);
                return 1;
            }

            result_count++;
        }

        free_entry(&e);
    }

    for (int i = 0; i < files_count; i++) {
        if (found[i] == 0) {
            printf("Внимание: файл '%s' не найден в архиве\n", get_base_name(files[i]));
        }
    }

    free(found);

	// исправляем кол - во файлов в архиве
    if (!patch_archive_count(temp, result_count)) {
        fclose(old);
        fclose(temp);
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    fclose(old);
    fclose(temp);

	// удаляем старый архив
    remove(archive_name);

	// переименовываем временный архив
    if (rename(temp_name, archive_name) != 0) {
        printf("Ошибка: не удалось переименовать временный архив\n");
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    free(temp_name);
    return 0;
}

// операция просмотра файлов в архиве -l
int list_archive(const char * archive_name) {
    FILE * fin = fopen(archive_name, "rb");

    if (fin == NULL) {
        printf("Ошибка: не удалось открыть архив '%s'.\n", archive_name);
        return 1;
    }

    unsigned int entries_count;

    if (!read_archive_header(fin, &entries_count)) {
        printf("Ошибка: не удалось прочитать заголовок архива при просмотре файлов\n");
        fclose(fin);
        return 1;
    }

    printf("Archive: %s\n", archive_name);
    printf("Files: %u\n\n", entries_count);
    printf("%-30s %12s %12s %12s\n", "Name", "Original", "Packed", "Ratio");

    for (unsigned int i = 0; i < entries_count; i++) {
        archive_entry e;
        e.name = NULL;

        if (!read_entry_header(fin, &e)) {
            printf("Ошибка: не удалось прочитать заголовок файла при просмотре файлов\n");
            fclose(fin);
            return 1;
        }

        unsigned int packed = get_tree_bytes(e.symbols_count) + e.data_size;	// сжатая файловая запись
        double ratio = 0.0;		// коэффициент сжатия

        if (e.original_size > 0) {
            ratio = (double)packed * 100.0 / (double)e.original_size;
        }

        printf("%-30s %12u %12u %11.2f%%\n", e.name, e.original_size, packed, ratio);

        if (!skip_bytes(fin, packed)) {
            printf("Ошибка: выход за границы файла\n");
            free_entry(&e);
            fclose(fin);
            return 1;
        }

        free_entry(&e);
    }

    fclose(fin);
    return 0;
}

// функция проверки архива -t
int test_archive(const char * archive_name) {
    FILE * fin = fopen(archive_name, "rb");

    if (fin == NULL) {
        printf("Ошибка: не удалось открыть архив '%s'.\n", archive_name);
        return 1;
    }

    unsigned int entries_count;

    if (!read_archive_header(fin, &entries_count)) {
        printf("Ошибка: не удалось прочитать заголовок архива\n");
        fclose(fin);
        return 1;
    }

    for (unsigned int i = 0; i < entries_count; i++) {
        archive_entry e;
        e.name = NULL;

        if (!read_entry_header(fin, &e)) {
            printf("Ошибка: не удалось прочитать заголовок файла\n");
            fclose(fin);
            return 1;
        }

        unsigned int real_crc = 0;

        if (!decode_current_entry(fin, &e, NULL, &real_crc)) {
            printf("Ошибка: не удалось декодировать файл '%s'. Архив поврежден\n", e.name);
            free_entry(&e);
            fclose(fin);
            return 1;
        }

        if (real_crc != e.crc) {
            printf("Ошибка: различие CRC для файла '%s'.\n", e.name);
            free_entry(&e);
            fclose(fin);
            return 1;
        }

        printf("Успешно для файла: %s\n", e.name);
        free_entry(&e);
    }

    printf("Архив в пордяке\n");
    fclose(fin);
    return 0;
}
