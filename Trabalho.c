#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#define N_PROCESSOS 3
#define SEC 1
#define MAX 5

int GLOBAL_DEVICE = -1;
int GLOBAL_TIMEOUT = -1;
int GLOBAL_HAS_SYSCALL = -1;
int GLOBAL_TERMINATED = -1;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Estrutura da fila
struct queue {
    char* nome;
    int items[N_PROCESSOS];
    int primeiro, ultimo;
};typedef struct queue Queue;

//Estrutura de PCB
struct pcb {
    int PC; 
    char state[200]; //3 Estados Executing, Blocked DX Op, Terminated
    int qttD1;
    int qttD2;
};typedef struct pcb PCB;

// Prototipos das funções
void SignalHandler(int sinal);
void Syscall(char Dx, char Op,char* shm);
void processo(char* shm, PCB* pcb, int id);
void InterruptController();
void initQueue(Queue* q, char* nome);
void printQueue( Queue* q);
int isFull( Queue* q);
int isEmpty( Queue* q);
void enqueue( Queue* q, int value);
int dequeue( Queue* q);
int peek( Queue* q);
int encontrarIndex(int vetor[], int tamanho, int valor);


int main(void) {
    Queue blocked_D1;
    Queue blocked_D2;
    Queue ready_processes;
    Queue exec_process;
    Queue terminated_process;
    int ProcessControlBlock;
    PCB *pPCB;
    char systemcall, *sc;
    int pidInterrupter;
    int pidProcesses[N_PROCESSOS]; 


    initQueue(&blocked_D1, "Blocked_1");
    initQueue(&blocked_D2, "Blocked_2");
    initQueue(&ready_processes, "Ready");
    initQueue(&exec_process, "Active");
    initQueue(&terminated_process, "Terminated");
    systemcall = shmget (IPC_PRIVATE, 2*sizeof(char), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR); //Shm para as informacoes de Device e Operation
    ProcessControlBlock = shmget (IPC_PRIVATE, N_PROCESSOS*sizeof(PCB), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
     
    pidInterrupter = fork();
    if (pidInterrupter == 0) { //Interrupter
        raise(SIGSTOP);
        InterruptController();  
        exit(0);
    } 
    else //KernelSim 
    { 
        if(pidInterrupter != 0)
        {
            signal(SIGUSR1, SignalHandler);  
            signal(SIGUSR2, SignalHandler);  
            signal(SIGTERM, SignalHandler);  
            signal(SIGTSTP, SignalHandler); 
            signal(SIGIO, SignalHandler);
        }
        for (int i = 0; i < N_PROCESSOS; i++) {
            pidProcesses[i] = fork();
            if (pidProcesses[i] == 0) //Filho
            {  
                pPCB = (PCB*) shmat (ProcessControlBlock, 0, 0);
                sc = (char*)shmat(systemcall,0,0); //Conecta os filhos com shm
                processo(sc, pPCB, i);  // Executa a função do processo
                exit(0);  // Saída do processo filho
            }
        }


        sc = (char*)shmat(systemcall,0,0); //Conecta o pai com shm
        pPCB = (PCB*) shmat (ProcessControlBlock, 0, 0);
        
        for(int i =0; i<N_PROCESSOS;i++) //Filhos criados, agora coloca na fila
        {
            pthread_mutex_unlock(&mutex);
            enqueue(&ready_processes, pidProcesses[i]);
            pthread_mutex_unlock(&mutex);
        }

        int current = dequeue(&ready_processes); 
        enqueue(&exec_process, current);
        kill(current, SIGCONT); //Processo Ativo
        kill(pidInterrupter, SIGCONT); //Interrupter Ativo
        
        printQueue(&ready_processes);
        printQueue(&exec_process);
        printQueue(&blocked_D1);
        printQueue(&blocked_D2);
        printQueue(&terminated_process);
        printf("\n");

        while (1) {
            if(isFull(&terminated_process))
                break;

            if (GLOBAL_DEVICE != -1) { //Tratamento interrupcao
                switch (GLOBAL_DEVICE) {
                    case 1: 
                        if (!isEmpty(&blocked_D1)) {
                            int released_process = dequeue(&blocked_D1);
                            int index = encontrarIndex(pidProcesses,N_PROCESSOS, released_process);
                            printf("Processo %d com index %d liberado do dispositivo 1\n", released_process, index);
                              printf("pPCB[index].PC 1 --- %d\n", pPCB[index].PC);
                            if(pPCB[index].PC >= MAX) //Processo deve terminar
                            {
                                 printf("entrou1\n");
                                enqueue(&terminated_process, released_process);
                                kill(released_process, SIGCONT);
                            }
                            else
                                enqueue(&ready_processes, released_process);
                            
                            
                        }
                        break;
                    case 2: 
                        if (!isEmpty(&blocked_D2)) {
                            int released_process = dequeue(&blocked_D2);
                            int index = encontrarIndex(pidProcesses,N_PROCESSOS, released_process);
                            printf("Processo %d com index %d liberado do dispositivo 2\n", released_process, index);
                            printf("pPCB[index].PC 2--- %d\n", pPCB[index].PC);
                            if(pPCB[index].PC >= MAX) //Processo deve terminar
                            {
                                printf("entrou2\n");
                                enqueue(&terminated_process, released_process);
                                kill(released_process, SIGCONT);
                            }
                            else
                                enqueue(&ready_processes, released_process);
                        }
                        break;    
                }
                GLOBAL_DEVICE = -1;
            }

            if(GLOBAL_TERMINATED == 1 && GLOBAL_HAS_SYSCALL == -1 && !isEmpty(&exec_process))
            {
                int terminated = dequeue(&exec_process);
                printf("dequeue terminater %d\n",terminated);
                enqueue(&terminated_process, terminated);
                printQueue(&terminated_process);
                printQueue(&exec_process);
                if(!isEmpty(&ready_processes))
                {
                    int next = dequeue(&ready_processes);
                    printf("dequeue next %d\n",next);
                    enqueue(&exec_process, next);
                }
                GLOBAL_TERMINATED = -1;
                GLOBAL_TIMEOUT = -1;
            }

            if(GLOBAL_TIMEOUT != -1 && GLOBAL_HAS_SYSCALL == -1)
            {
                if (!isEmpty(&exec_process)) {
                    kill(peek(&exec_process), SIGSTOP); 
                    int current_process = dequeue(&exec_process);
                    enqueue(&ready_processes, current_process);
                    int next_process = peek(&ready_processes);
                    if (next_process != -1) {
                        dequeue(&ready_processes);
                        enqueue(&exec_process, next_process);
                        kill(next_process, SIGCONT); 
                    }
                }
                GLOBAL_TIMEOUT = -1;
            }


            if(GLOBAL_HAS_SYSCALL == 1)
            {
                char Dx = sc[0];
                char Op = sc[1];
                printf("DEVICE %c / OP %c\n", Dx, Op);
                int pidsyscall = dequeue(&exec_process); //Tira o processo q fez a syscall da fila
                kill(pidsyscall, SIGSTOP); //Para ele
                if (Dx == '1') { //Atribui a uma fila de blocked
                    enqueue(&blocked_D1, pidsyscall);
                } else if (Dx == '2') {
                    enqueue(&blocked_D2, pidsyscall);
                }
                int next_process = peek(&ready_processes); //Ativa o proximo da fila 
                if (next_process != -1) {
                    dequeue(&ready_processes);
                    enqueue(&exec_process, next_process);
                    kill(next_process, SIGCONT); 
                }

                GLOBAL_HAS_SYSCALL = -1;
            }

            if(isEmpty(&exec_process) && !isEmpty(&ready_processes))
            {
                int next_process = peek(&ready_processes);
                    if (next_process != -1) {
                        dequeue(&ready_processes);
                        enqueue(&exec_process, next_process);
                        kill(next_process, SIGCONT); 
                    }
            }
            printf("-----------------------------------------------\n");
            printQueue(&ready_processes);
            printf("\n");
            printQueue(&exec_process);
            printf("\n");
            printQueue(&blocked_D1);
            printQueue(&blocked_D2);
            printQueue(&terminated_process);
            printf("\n");
            printf("-----------------------------------------------\n");
            sleep(1); // Pausa no loop para evitar consumo excessivo de CPU
        }
    }

    for (int i = 0; i < N_PROCESSOS; i++) {
        wait(NULL);
    }

    shmctl(systemcall,IPC_RMID, 0);
    shmctl(ProcessControlBlock,IPC_RMID, 0);
    printf("CAbO PORRA\n");
    return 0;
}

// Funções de controle de interrupção e syscalls
void SignalHandler(int sinal) {
    switch (sinal) {
        case SIGUSR1:
            printf("Device 1 liberado\n");
            GLOBAL_DEVICE = 1;
            break;
        case SIGUSR2: 
            printf("Device 2 liberado\n");
            GLOBAL_DEVICE = 2;
            break;
        case SIGTERM: 
            printf("Timeout\n");
            GLOBAL_TIMEOUT = 0;
            break;
        case SIGTSTP:
            printf("Syscall\n");
            GLOBAL_HAS_SYSCALL = 1;
        case SIGIO:
            printf("Terminated\n");
            GLOBAL_TERMINATED = 1;
    }
}

void Syscall(char Dx, char Op, char* shm) {
    shm[0] = Dx;
    shm[1] = Op;
}

void processo(char* shm, PCB* pcb, int id) {
    int PC = 0;
    int d;
    char Dx;
    char Op;
    int f;

    pthread_mutex_lock(&mutex);
    pcb[id].PC = PC; 
    pcb[id].qttD1 = 0;
    pcb[id].qttD2 = 0;  
    pthread_mutex_unlock(&mutex);

    raise(SIGSTOP);
    srand ( time(NULL) );

    while (PC < MAX) {
        pthread_mutex_lock(&mutex);
        pcb[id].PC = PC; 
        pthread_mutex_unlock(&mutex);
        sleep(1);
        printf("Processo executando: %d PC: %d\n", getpid(), PC);
        d = rand();
        f  = (d % 100) + 1;
        if (f < 20) { 
            printf("SYSCALL PROCESSO %d\n", getpid());
            if (d % 2) 
            {
                Dx = '1';
                pthread_mutex_lock(&mutex);
                pcb[id].qttD1++;
                pthread_mutex_unlock(&mutex);
            }
            else 
            {
                Dx = '2';
                pthread_mutex_lock(&mutex);
                pcb[id].qttD2++;
                pthread_mutex_lock(&mutex);

            }

            if (d % 3 == 1) 
                Op = 'R';
            else if (d % 3 == 1) 
                Op = 'W';
            else 
                Op = 'X';
            Syscall(Dx, Op, shm);   
            kill(getppid(), SIGTSTP);
        }
        sleep(1);
        PC++;

    }
    pcb[id].PC = PC; 

    printf("Processo %d terminated com PC %d\n", getpid(), pcb[id].PC);

    shmdt(pcb);
    shmdt(shm);
    kill(getppid(), SIGIO);
    //terminou
    //printf("------------- PC: %d, QTTD1: %d, QTTD2: %d\n",pcb[id].PC,pcb[id].qttD1,pcb[id].qttD2);
}

void InterruptController() {
    int parent_id = getppid();
    srand(time(NULL));   

    while (1) {
        double random_num = (((double) rand()) / RAND_MAX);
        if ( random_num <= 0.3) {
            kill(parent_id, SIGUSR1);
        }
        else if (random_num <= 0.6) {
            kill(parent_id, SIGUSR2);
        }
        usleep(SEC * 1000000);
        kill(parent_id, SIGTERM);
    }
}

// Funções relacionadas à fila
void initQueue(Queue* q, char* nome) {
    q->nome = nome;
    q->primeiro = -1;
    q->ultimo = -1;
    printf("\n_____ Na Init ______\n\n");
    printQueue(q);
}

void printQueue(Queue* q) {
    if (isEmpty(q)) {
        printf("A fila %s está vazia.\n", q->nome);
        return;
    }

    printf("Fila %s: ", q->nome);
    int i = q->primeiro;
    while (i != q->ultimo) {
        printf("%d ", q->items[i]);
        i = (i + 1) % N_PROCESSOS;  // Avança de forma circular
    }
    printf("%d\n", q->items[q->ultimo]);  // Imprime o último elemento
}


int isFull( Queue* q) {
    return (q->ultimo + 1) % N_PROCESSOS == q->primeiro;
}


int isEmpty( Queue* q) {
    return q->primeiro == -1;
}

void enqueue( Queue* q, int value) {
    if (isFull(q)) {
        printf("-----------------Fila cheia!------------------------\n");
    } else {
        if (isEmpty(q)) {
            q->primeiro = 0;  // Define o índice inicial
        }
        q->ultimo = (q->ultimo + 1) % N_PROCESSOS;  // Incrementa de forma circular
        q->items[q->ultimo] = value;
        printf("Inserido %d na fila %s\n", value, q->nome);
    }
}


int dequeue( Queue* q) {
    int item;
    if (isEmpty(q)) {
        printf("Fila vazia!\n");
        return -1;
    } else {
        item = q->items[q->primeiro];
        q->primeiro = (q->primeiro + 1) % N_PROCESSOS;  // Avança de forma circular
        if (q->primeiro == (q->ultimo + 1) % N_PROCESSOS) {
            // Se a fila estiver vazia após a remoção
            q->primeiro = q->ultimo = -1;
        }
        printf("\nRemovido %d da fila %s\n\n", item, q->nome);
        return item;
    }
}


int peek( Queue* q) {
    if (isEmpty(q)) {
        return -1;
    }
    return q->items[q->primeiro];
}

int encontrarIndex(int vetor[], int tamanho, int valor) {
    for (int i = 0; i < tamanho; i++) {
        if (vetor[i] == valor) {
            return i;  // Retorna o índice do valor encontrado
        }
    }
    return -1;  // Retorna -1 se o valor não for encontrado
}
