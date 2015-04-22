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

static const ssize_t NO_TASK = -1;
static const int CHILDREN_NUMBER = 10;

static struct sembuf sop_get_task_lock[2] = {
    {0, 0, 0}, //ожидать освобождения
    {0, 1, 0} //занять семафор
};
static struct sembuf sop_get_task_unlock[1] = {
    {0, -1, 0}
};

static struct sembuf sop_write_res_lock[2] = {
    {1, 0, 0}, //ожидать освобождения
    {1, 1, 0}  //занять семафор
};
static struct sembuf sop_write_res_unlock[1] = {
    {1, -1, 0}
};

size_t file_length;
size_t tasks_num;
int semaphores;

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
    printf("File content:\n%s\n", data);
    shmdt(data);
    return shm_key;
}

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
        exit(EXIT_FAILURE);
    }

  //разместим задачи в разделяемой памяти
    ssize_t* tasks_mem = (ssize_t*) mount_shm(shm_key);
    memcpy(tasks_mem, tasks, tasks_num * sizeof(ssize_t));
    printf("Tasks are:\n");
    for (size_t i = 0; i < tasks_num; ++i)
    {
        printf("%d\n", tasks_mem[i]);
    }
    shmdt(tasks_mem);
    free(tasks);
    return shm_key;
}

int init_semaphores(void)
{
  //создадим группу из двух семафоров, 
  //0 - на чтение задачи, 1 - на запись результата
    int sem_group =  semget(IPC_PRIVATE, 2, 
        IPC_CREAT | 0660);
    return sem_group;

}

ssize_t get_next_task(ssize_t* tasks_mem)
{
  //блокируем доступ других детей
    semop(semaphores, &sop_get_task_lock, 2);
    ssize_t next_task = NO_TASK;
  //ищем нерешенную задачу
    for (size_t i = 0; i < tasks_num; ++i)
    {
        if (tasks_mem[i] != NO_TASK)
        {
            next_task = tasks_mem[i];
            tasks_mem[i] = NO_TASK;
            break;
        }
    }
  //разблокируем доступ к задачам  
    semop(semaphores, &sop_get_task_unlock, 1);
    return next_task;
}

char* process_string(char* str, int child_id)
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
    printf("(%d) result is: %s\n", child_id, result);
}

int init_result_buffer(void)
{
  //создаем область разделяемой памяти для результатов
  //shmget гарантирует инициалиизацию нулями
    int shm_key = shmget(IPC_PRIVATE, 
      //указатель на конец текущего буфера
        sizeof(char*)
      //для каждого задания указатель на начало и конец результата в буфере
        + tasks_num * 2 * sizeof(char*) 
      //сам буфер, в два раза больше, чем исходный файл
        + file_length * 2, 
        IPC_CREAT | 0660);

    if (shm_key == -1)
    {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

    return shm_key;
}

void write_result_to_buffer(size_t task_id, char* answer)
{
    semop(semaphores, &sop_write_res_lock, 2);
    semop(semaphores, &sop_write_res_unlock, 1);
}

int main(int argc, char** argv)
{
    int file_shm_key = read_file(argc, argv);
    int tasks_shm_key = init_tasks(file_shm_key);
    int result_buf = init_result_buffer();
    semaphores = init_semaphores();

    for (int i = 0; i < CHILDREN_NUMBER; ++i)
    {
        pid_t code = fork();
        if (code == 0)
        {
            ssize_t* tasks_mem = (ssize_t*) mount_shm(tasks_shm_key);
            char* file_mem = (char*) mount_shm(file_shm_key); 
            ssize_t line_offset;

            do
            {
                line_offset = get_next_task(tasks_mem);

                printf("(%d) get %d - %s\n", i, 
                       line_offset, file_mem + line_offset);

                process_string(file_mem + line_offset, i);
            }while(line_offset != NO_TASK);

            _exit(EXIT_SUCCESS);
        }
    }

    for (int i = 0; i < CHILDREN_NUMBER; ++i)
    {
        wait(NULL);
    }
    return 0;
}
