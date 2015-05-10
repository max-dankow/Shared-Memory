#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/mman.h>
#include <string.h>
#include <semaphore.h>

static const ssize_t NO_TASK = -1;
static const int CHILDREN_NUMBER = 10;

size_t file_length;
size_t tasks_num;
size_t total_size;
sem_t sem_get_tasks;
sem_t sem_write_res;

char* mmap_file(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Wrong arguments\n");
        exit(EXIT_FAILURE);
    }
    struct stat info;
    if (stat(argv[1], &info) == -1)
    {
        perror("Can't get file info.\n");
        exit(EXIT_FAILURE);
    }
    file_length = info.st_size;
    char* text = (char*) malloc(file_length + 1);
    int real_file = open(argv[1], O_RDONLY, 0777);
    int code = read(real_file, text, file_length);
    return text;
}

/*
Возвращает идентификатор разделяемой памяти,
в которую записывает индексы начал строк,
также записывает число задач в переменную tasks_num.
*/
ssize_t* init_tasks(char* file_ptr)
{
  //посчитаем число строк-задач  
    char* data = file_ptr;
    tasks_num = 1;
    for (size_t i = 0; i < file_length; ++i)
    {
        if (data[i] == '\n')
        {
            tasks_num++;
        }
    }
  //создадим область разделяемой памяти для заданий
    int shm = shm_open("/tasks", O_RDWR | O_CREAT, 0666);
    ftruncate(shm, tasks_num * sizeof(ssize_t));
    ssize_t* tasks = mmap(NULL, tasks_num * sizeof(ssize_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    if (tasks == MAP_FAILED)
    {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
  //найдем индексы начала строк (массив tasks)
    size_t task_index = 1;
    tasks[0] = 0;
    for (size_t i = 0; i < file_length; ++i)
    {
        if (data[i] == '\n')
        {
            tasks[task_index] = i + 1;
            data[i] = '\0';
            ++task_index;
        }
    }
    return tasks;
}

ssize_t get_next_task(ssize_t* tasks_ptr, ssize_t *offset)
{
  //блокируем доступ других детей
    sem_wait(&sem_get_tasks);
    ssize_t next_task = NO_TASK;
    *offset = NO_TASK;
  //ищем нерешенную задачу
    for (size_t i = 0; i < tasks_num; ++i)
    {
        if (tasks_ptr[i] != NO_TASK)
        {
            next_task = i;
            *offset = tasks_ptr[i];
            tasks_ptr[i] = NO_TASK;
            break;
        }
    }
  //разблокируем доступ к задачам  
    sem_post(&sem_get_tasks);
    return next_task;
}

void write_result_to_buffer(size_t* result_ptr, size_t task_id, char* answer)
{
    sem_wait(&sem_write_res);
    size_t buf_end = result_ptr[0];
    size_t len = strlen(answer);
  //сдвигаем 'указатель' свободного блока  
    result_ptr[0] += len; 
    sem_post(&sem_write_res);
    memcpy((char*) result_ptr + buf_end, answer, len);
  //указываем в заголовке, где 'лежит' строка
    result_ptr[1 + task_id * 2] = buf_end;
    result_ptr[1 + task_id * 2 + 1] = buf_end + len;
}

char* process_string(char* str)
{
    char* result = malloc(strlen(str) * 2 + 1);
    char* index = result;
    while (*str != '\0')
    {
        if (*str >= 'A' && *str <= 'Z')
        {
            *index = (char) tolower(*str);
            ++index;
            *index = (char) tolower(*str);
            ++index;
        }
        else
        {
            if (*str >= 'a' && *str <= 'z')
            {
                *index = (char) toupper(*str);
                ++index;
            }
            else
            {
                *index = *str;
                ++index;
            }    
        }
        ++str;
    }
    *index = '\0';
    return result;
}

void* init_result_buffer(void)
{
    int shm = shm_open("/output", O_RDWR | O_CREAT, 0666);
    total_size = (sizeof(size_t)//указатель на конец текущего буфера
                 + tasks_num * 2 * sizeof(size_t) //для каждого задания указатель на начало и конец результата в буфере
                 + file_length * 2);//сам буфер, в два раза больше, чем исходный файл
    ftruncate(shm, total_size);
    void* answers = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);

  //инициализируем 'указатель' свободного блока смещением первого байта после заголовка
    size_t* header = (size_t*) answers;
    header[0] = sizeof(size_t) + tasks_num * 2 * sizeof(size_t);
    return answers;
}

void print_result(char* result_ptr)
{
    size_t* header = (size_t*) result_ptr + 1;
    for (size_t i = 0; i < tasks_num; ++i)
    {
        write(1, result_ptr + header[i * 2], header[i * 2 +  1] - header[i * 2]);
        printf("\n");
    }
}

int main(int argc, char** argv)
{
  //создаем 3 области разделяемой памяти
    char* file_ptr = mmap_file(argc, argv);
    ssize_t* tasks_ptr = init_tasks(file_ptr);
    void* result_ptr = init_result_buffer(); 

    if (sem_init(&sem_get_tasks, 0, 1) == -1 || sem_init(&sem_write_res, 0, 1))
    {
        perror("sem_init");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < CHILDREN_NUMBER; ++i)
    {
        pid_t code = fork();
        if (code == 0)
        {
            ssize_t line_offset;
          //пока есть задания
            while (1)
            {
                ssize_t offset;
                ssize_t task_id = get_next_task(tasks_ptr, &offset);
                if (task_id == NO_TASK)
                {
                    break;
                }
                char* answer = process_string(file_ptr + offset);
                write_result_to_buffer(result_ptr, task_id, answer);
                free(answer);
            }
            free(file_ptr);
            munmap(tasks_ptr, tasks_num * sizeof(ssize_t));
            munmap(result_ptr, total_size);
            _exit(EXIT_SUCCESS);
        }
    }
    for (int i = 0; i < CHILDREN_NUMBER; ++i)
    {
        wait(NULL);
    }
    print_result(result_ptr);
    free(file_ptr);
    munmap(tasks_ptr, tasks_num * sizeof(ssize_t));
    munmap(result_ptr, total_size);
    shm_unlink("/tasks");
    shm_unlink("/output");
    sem_destroy(&sem_get_tasks);
    sem_destroy(&sem_write_res);
    return 0;
}
