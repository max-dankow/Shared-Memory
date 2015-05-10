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
sem_t sem_get_tasks;
sem_t sem_write_res;

void* mount_shm(int shm_key)
{
    void* shm_ptr = shmat(shm_key, NULL, 0);
    if (shm_ptr == (void*) -1)
    {
        perror("shmap");
        exit(EXIT_FAILURE);
    }
    return shm_ptr;
}

/*
Возвращает идентификатор разделяемой памяти,
в которую целиком записывает содержимое файла argv[1]
помещает ее размер в глобальную переменную file_length.
*/
int read_file(int argc, char** argv)
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

    int shm_key = shmget(IPC_PRIVATE, 
        file_length + 1, IPC_CREAT | 0660);

    if (shm_key == -1)
    {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

    char* data = (char*) mount_shm(shm_key);
    int real_file = open(argv[1], O_RDONLY, 0777);
    read(real_file, data, file_length);
    shmdt(data);
    return shm_key;
}

/*
Возвращает идентификатор разделяемой памяти,
в которую записывает индексы начал строк,
также записывает число задач в переменную task_num.
*/
int init_tasks(int file_shm_key)
{
  //посчитаем число строк-задач  
    char* data = shmat(file_shm_key, NULL, 0);
    if (data == (void*) -1)
    {
        perror("shmap");
        exit(EXIT_FAILURE);
    }

    tasks_num = 1;
    for (size_t i = 0; i < file_length; ++i)
    {
        if (data[i] == '\n')
        {
            tasks_num++;
        }
    }
  //найдем индексы начала строк (массив tasks)
    ssize_t* tasks = malloc(tasks_num * sizeof(ssize_t));
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
  //создадим область разделяемой памяти для заданий
    int shm_key = shmget(IPC_PRIVATE, 
        file_length + 1, IPC_CREAT | 0660);

    if (shm_key == -1)
    {
        perror("shmget");
        free(tasks);
        exit(EXIT_FAILURE);
    }

  //разместим задачи в разделяемой памяти
    ssize_t* tasks_ptr = (ssize_t*) mount_shm(shm_key);
    memcpy(tasks_ptr, tasks, tasks_num * sizeof(ssize_t));
    shmdt(tasks_ptr);
    free(tasks);
    return shm_key;
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

int init_result_buffer(void)
{
  //создаем область разделяемой памяти для результатов
  //shmget гарантирует инициализацию памяти нулями
    int shm_key = shmget(IPC_PRIVATE, 
      //указатель на конец текущего буфера
        sizeof(size_t)
      //для каждого задания указатель на начало и конец результата в буфере
        + tasks_num * 2 * sizeof(size_t) 
      //сам буфер, в два раза больше, чем исходный файл
        + file_length * 2, 
        IPC_CREAT | 0660);

    if (shm_key == -1)
    {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

  //инициализируем 'указатель' свободного блока смещением первого байта после заголовка
    size_t* header = (size_t*) mount_shm(shm_key);
    header[0] = sizeof(size_t) + tasks_num * 2 * sizeof(size_t);
    shmdt(header);
    return shm_key;
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
    int file_shm_key = read_file(argc, argv);
    int tasks_shm_key = init_tasks(file_shm_key);
    int result_shm_key = init_result_buffer();
    
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
            ssize_t* tasks_ptr = (ssize_t*) mount_shm(tasks_shm_key);
            char* file_ptr = (char*) mount_shm(file_shm_key); 
            void* result_ptr = mount_shm(result_shm_key);
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
            shmdt(tasks_ptr);
            shmdt(file_ptr);
            shmdt(result_ptr);
            _exit(EXIT_SUCCESS);
        }
    }

    for (int i = 0; i < CHILDREN_NUMBER; ++i)
    {
        wait(NULL);
    }

    void* answer_mem = mount_shm(result_shm_key);
    print_result(answer_mem);
    shmdt(answer_mem);

    shmctl(file_shm_key, IPC_RMID, 0);
    shmctl(tasks_shm_key, IPC_RMID, 0);
    shmctl(result_shm_key, IPC_RMID, 0);
    sem_destroy(&sem_get_tasks);
    sem_destroy(&sem_write_res);
    return 0;
}
