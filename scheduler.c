//DEFINING CONSTRAINTS
#define MAX_DOCKS 30
#define MAX_DOCK_CATEGORY 25
#define MAX_SHIP_CATEGORY 25
#define MAX_SOLVERS 8
#define MAX_CRANE_CAPACITY 30
#define MAX_REGULAR_SHIPS 500
#define MAX_EMERGENCY_SHIPS 100
#define MAX_OUTGOING_SHIPS 500
#define MAX_CARGO_COUNT 200
#define MAX_REQUESTS_PER_TIMESTEP 100
#define MAX_AUTH_STRING_LEN 100
#define MAX_NEW_REQUESTS 100
#define MAX 100
#define SIZE 1000

//LIBRARIES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <stddef.h>
#include <math.h>
#include <pthread.h>

int TimeStep;

//GLOBAL ARRAYS
int dock_status[MAX_DOCKS];
int docks_cat[MAX_DOCKS];
int lastHandledCargoTs[MAX_DOCKS];
int readyToUndock[MAX_DOCKS];
int solverQ[MAX_SOLVERS];

//VALIDATING INPUT FILE
void validate_input_file(int argc, char *argv[], char *filepath) {
if (argc < 2) {
fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
exit(EXIT_FAILURE);
}
int testCaseNum = atoi(argv[1]);
sprintf(filepath, "testcase%d/input.txt", testCaseNum);
FILE *file = fopen(filepath, "r");
if (!file) {
perror("Error opening input file");
exit(EXIT_FAILURE);
}
fclose(file);
}

//DEFINING STRUCTURES
typedef struct MessageStruct {
long mtype;
int timestep;
int shipId;
int direction;
int dockId;
int cargoId;
int isFinished;
union {
int numShipRequests;
int craneId;
};
} MessageStruct;

typedef struct ShipRequest{
int shipId;
int timestep;
int category;
int direction;
int emergency;
int waitingTime;
int numCargo;
int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct {
    ShipRequest requests[SIZE];
    int fr;
    int rr;
} structure;

typedef struct MainSharedMemory {
char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

MainSharedMemory* shared_memory = NULL;  //GLOBAL DEFINITION

char allowed_chars[] = { '5', '6', '7', '8', '9', '.' };
int charset_len = sizeof(allowed_chars) / sizeof(char);

typedef struct SolverRequest{
long mtype;
int dockId;
char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse{
long mtype;
int guessIsCorrect;
} SolverResponse;

//QUEUE FUNCTIONS
void InitQ(structure* q) {
    q->fr = -1;
    q->rr = -1;
}

void enqueue(structure* q, ShipRequest value) {
    if (q->rr == SIZE - 1) {
        printf("Queue is full!\n");
        return;
    }

    if (q->fr == -1) {
        q->fr = 0;
    }

    q->rr++;
    q->requests[q->rr] = value;
}

ShipRequest dequeue(structure* q) {
    ShipRequest dummy = {0}; 

    if (q->fr == -1) {
        printf("Queue is empty!\n");
        return dummy;
    }

    ShipRequest item = q->requests[q->fr];

    if (q->fr == q->rr) {
        q->fr = q->rr = -1;
    } else {
        q->fr++;
    }

    return item;
}

int queueCount(structure* q) {
    if (q->fr == -1) return 0;
    return q->rr - q->fr + 1;
}

ShipRequest delete(structure* q, int index) {
    ShipRequest dummy = {0};

    if (q->fr == -1|| index < q->fr || index > q->rr) {
        printf("Invalid index!\n");
        return dummy;
    }

    ShipRequest del;
    memcpy(&del, &q->requests[index], sizeof(ShipRequest));

    for (int i = index; i < q->rr; i++) {
        q->requests[i] = q->requests[i + 1];
    }

    q->rr--;

    if (q->rr < q->fr) {
        q->fr = q->rr = -1;
    }

    return del;
}

ShipRequest ships_in_docks[MAX_DOCKS];

int main(int argc, char *argv[]) {

//OPENING INPUT FILE
char filepath[100];
validate_input_file(argc, argv, filepath);

FILE *fp = fopen(filepath, "r");
if (!fp) {
perror("Error opening input file");
return 1;
}

//READING INPUT FILE ELEMENTS
int shm_key, main_msgq_key;
int m, solver_msgq_keys[MAX_SOLVERS];
int n;
int dock_categories[MAX_DOCKS];
int dock_cranes[MAX_DOCKS][MAX_DOCK_CATEGORY];
fscanf(fp, "%d", &shm_key);
fscanf(fp, "%d", &main_msgq_key);

fscanf(fp, "%d", &m);
for (int i = 0; i < m; i++) {
fscanf(fp, "%d", &solver_msgq_keys[i]);
}

fscanf(fp, "%d", &n);
for (int i = 0; i < n; i++) {
fscanf(fp, "%d", &dock_categories[i]);
for (int j = 0; j < dock_categories[i]; j++) {
fscanf(fp, "%d", &dock_cranes[i][j]);
}
docks_cat[i] = dock_categories[i]; 
}

//IPC MECHANISM - MAIN MESSAGE QUEUE
int main_msgq_id = msgget(main_msgq_key, 0644 | IPC_CREAT);
if (main_msgq_id == -1) {
perror("Scheduler: Failed to connect to main message queue");
exit(EXIT_FAILURE);
} else {
printf("Scheduler: Connected to main message queue ( main_msgq_id: %d)\n", main_msgq_id);
}

//IPC MECHANISM - SOLVER MESSAGE QUEUE
for (int i = 0; i < m; i++) {
solverQ[i] = msgget(solver_msgq_keys[i], 0666 | IPC_CREAT);
if (solverQ[i] == -1) {
fprintf(stderr, "Scheduler: Failed to connect to solver %d message queue (solver_msgq_keys: %d): ", i, solver_msgq_keys[i]);
perror("");
exit(EXIT_FAILURE);
} else {
printf("Scheduler: Connected to solver %d message queue (id: %d)\n", i, solverQ[i]);
}
}

//IPC MECHANISM - SHARED MEMORY
int shmid = shmget(shm_key, sizeof(MainSharedMemory), 0666);
if (shmid == -1) {
perror("Scheduler: Failed to get shared memory segment");
exit(EXIT_FAILURE);
}

//ATTACHING SHARED MEMORY
shared_memory = (MainSharedMemory *)shmat(shmid, NULL, 0);
if (shared_memory == (void *) -1) {
perror("Scheduler: Failed to attach shared memory");
exit(EXIT_FAILURE);
}

//INITIALIZE AUTH STRINGS TO EMPTY STRINGS
for (int i = 0; i < MAX_DOCKS; i++) {
    memset(shared_memory->authStrings[i], 0, MAX_AUTH_STRING_LEN); 
}

printf("Scheduler: Successfully attached to shared memory (shmid: %d)\n", shmid);

//INITIALIZING QUEUES
    structure Emergency;
    structure Regular;
   InitQ(&Emergency);
    InitQ(&Regular);
    MessageStruct shipMsg;
    
      int dock_recent[n];
        int just_unloaded[n];
    
//INITIALIZING ARRAYS    
    for(int i=0;i<n;i++){
        lastHandledCargoTs[i] = -1;
        dock_recent[i] = 0; 
        just_unloaded[i] = 0;
        readyToUndock[i] = 0;
        dock_status[i] = 0;
    }

while(shipMsg.isFinished != 1){
    
    if(msgrcv(main_msgq_id, &shipMsg, sizeof(shipMsg) - sizeof(long),1,0) == -1){
            printf("Failed to receive message from Validator\n");
            exit(0);
        }
        TimeStep = shipMsg.timestep;
        printf("Timestep: %d\n",TimeStep);
        int numShipReq = shipMsg.numShipRequests;
        printf("Number of Ship Requests:  %d\n",numShipReq);

//ENQUEUING EMERGENCY AND NORMAL
    for(int i =0 ; i<numShipReq; i++){
            if (shared_memory->newShipRequests[i].emergency == 1){
                enqueue(&Emergency,shared_memory->newShipRequests[i]);
                printf("Timestep- %d, shipID- %d, Type : Emergency\n",shared_memory->newShipRequests[i].timestep,shared_memory->newShipRequests[i].shipId);
            }
            else{
                enqueue(&Regular,shared_memory->newShipRequests[i]);
                printf("Timestep- %d, shipID- %d, Type : Regular\n",shared_memory->newShipRequests[i].timestep,shared_memory->newShipRequests[i].shipId);
            
            }
        }
    
memset(dock_recent, 0, sizeof(int) * n);

//DOCKING
void docking(ShipRequest* ship, int dock, int msg_que) {
    MessageStruct dockingmessage;
    memset(&dockingmessage, 0, sizeof(dockingmessage)); 

    dockingmessage.mtype = 2;
    dockingmessage.dockId = dock;
    dockingmessage.shipId = ship->shipId;
    dockingmessage.direction = ship->direction;

    size_t msgSize = sizeof(dockingmessage) - sizeof(dockingmessage.mtype);

    int status = msgsnd(msg_que, &dockingmessage, msgSize, 0);
    if (status == -1) {
        perror("Failed to send docking message to Validator");
        exit(1);
    }

    ships_in_docks[dock] = *ship;
    ships_in_docks[dock].timestep = TimeStep;

    printf("Dock %d assigned to Ship %d\n", dock, ship->shipId);
}


void process_ship_queue(structure* q, const char* type, int msg_queue, int dock_recent[], int dock_status[],int n) {
    for (int i = 0; i < queueCount(q); i++) {
        ShipRequest* ship = &q->requests[i];

        int selectedDock = -1;
        int minCategory = 1e9;

        for (int dockid = 0; dockid < n; dockid++) {
            if (!dock_status[dockid] && docks_cat[dockid] >= ship->category && docks_cat[dockid] < minCategory) {
                minCategory = docks_cat[dockid];
                selectedDock = dockid;
            }
        }

        if (selectedDock != -1) {
            if (strcmp(type, "Regular") == 0 && ship->direction == 1 && TimeStep > ship->timestep + ship->waitingTime) {
                printf("Timeout: Ship %d exceeded the allowed waiting period %d\n", ship->shipId, ship->timestep + ship->waitingTime);
                delete(q, i);
                i--;
            } else {
                printf("Ship %d is now docked at Dock %d\n", ship->shipId,selectedDock);
                docking(ship, selectedDock, msg_queue);
                delete(q, i);
                dock_status[selectedDock] = 1;
                dock_recent[selectedDock] = 1;
                i--;
            }
        }
    }
}

//ASSIGNING THE BEST DOCKS   
process_ship_queue(&Emergency,"Emergency",main_msgq_id,dock_recent,dock_status,n);
process_ship_queue(&Regular,"Regular",main_msgq_id,dock_recent,dock_status,n);

//LOADING-UNLOADING
void cargoHandling(int dock, int msg_que, int dock_cranes[][MAX_DOCK_CATEGORY]) {
    printf("Handling cargo at dock %d for Category %d\n", dock, docks_cat[dock]);

    MessageStruct msg = {0};
    msg.mtype = 4;
    msg.dockId = dock;
    msg.shipId = ships_in_docks[dock].shipId;
    msg.direction = ships_in_docks[dock].direction;

    int used_cargo[MAX_CARGO_COUNT] = {0};

    int numCrane = docks_cat[dock];
    int numCargo = ships_in_docks[dock].numCargo;

    for (int craneId = 0; craneId < numCrane; craneId++) {
        int crane_cap = dock_cranes[dock][craneId];

        int chosenCargoId = -1;
        int leastWastedCapacity = 1e9;

        for (int cargoId = 0; cargoId < numCargo; cargoId++) {
            int weight = ships_in_docks[dock].cargo[cargoId];

            if (weight == 0 || used_cargo[cargoId] || weight > crane_cap) {
                continue;
            }

            int waste = crane_cap - weight;
            if (waste < leastWastedCapacity) {
                leastWastedCapacity = waste;
                chosenCargoId = cargoId;
            }
        }

        if (chosenCargoId != -1) {
            msg.craneId = craneId;
            msg.cargoId = chosenCargoId;

            if (msgsnd(msg_que, &msg, sizeof(msg) - sizeof(msg.mtype), 0) == -1) {
                perror("Failed to send cargo handling message");
                exit(1);
            }

            used_cargo[chosenCargoId] = 1;
            ships_in_docks[dock].cargo[chosenCargoId] = 0;
            lastHandledCargoTs[dock] = TimeStep;

            printf("Ship %d with- cargo_id %d,item-%d - assigned to crane %d of Capacity %d\n",
                   ships_in_docks[dock].shipId,
                   
                   chosenCargoId,
                   ships_in_docks[dock].numCargo,
                   
            
                   craneId,
                   dock_cranes[dock][craneId]);
        }
    }
}

void process_docks_for_loading(int msg_queue, int dock_recent[], int n,int dock_cranes[][MAX_DOCK_CATEGORY]) {
int dockid=0;
    while(dockid < n) {
        if (dock_status[dockid] && !dock_recent[dockid]) {
            int cargo_left = 0;
            for (int i = 0; i < ships_in_docks[dockid].numCargo; i++) {
                if (ships_in_docks[dockid].cargo[i] != 0) {
                    cargo_left = 1;
                    break;
                }
            }

            if (cargo_left) {
                printf("Executing Loading/Unloading \n");
                cargoHandling(dockid, msg_queue,dock_cranes);
            } else if (lastHandledCargoTs[dockid] != -1 && TimeStep > lastHandledCargoTs[dockid]) {
                printf("Ship %d successfully unloaded at timestep %d \n", ships_in_docks[dockid].shipId, lastHandledCargoTs[dockid]);
                readyToUndock[dockid] = 1;
            }
        }
        dockid++;
    }
}

//UNDOCKING
void undocking(int dockid, int main_msg_queue, MainSharedMemory *shared_address,int no_solvers) {
    printf("Undocking process started for Dock %d...\n", dockid);

    int shipId = ships_in_docks[dockid].shipId;
    int direction = ships_in_docks[dockid].direction;
    int dock_ts = ships_in_docks[dockid].timestep;
    int len = lastHandledCargoTs[dockid] - dock_ts;

    if (len <= 0 || len >= MAX_AUTH_STRING_LEN) {
        printf("Invalid authentication string length %d provided for Dock %d\n", len, dockid);
        return;
    }

    printf("Authentication string for Dock %d has length %d\n", dockid, len);

    const char charset[] = { '5', '6', '7', '8', '9', '.' };
    const int charset_len = 6;
    char current_guess[MAX_AUTH_STRING_LEN];
    SolverResponse resp;
    SolverRequest req;
    req.mtype = 2;
    req.dockId = dockid;

    SolverRequest dock_notify = {.mtype = 1, .dockId = dockid};
    for (int i = 0; i < no_solvers; i++) {
        if (msgsnd(solverQ[i], &dock_notify, sizeof(dock_notify) - sizeof(long), 0) == -1) {
            perror("Solver notification failed");
        }
    }

    int found = 0;
    unsigned long long int guess_count = 0;

    void generate_and_try(int pos) {
        if (found) return;
        if (pos == len) {
            current_guess[len] = '\0';

            // Check first and last character validity
            if (current_guess[0] == '.' || current_guess[len - 1] == '.') return;

            // Send to solver (round-robin over solvers)
            int solver_idx = guess_count % no_solvers;
            guess_count++;

            strcpy(req.authStringGuess, current_guess);
            if (msgsnd(solverQ[solver_idx], &req, sizeof(req) - sizeof(long), 0) == -1) {
                perror("Message sending failed");
                return;
            }

            if (msgrcv(solverQ[solver_idx], &resp, sizeof(resp) - sizeof(long), 3, 0) == -1) {
                perror("Message receiving failed");
                return;
            }

            if (resp.guessIsCorrect == 1) {
                printf("Dock %d- retrieved Auth string %s\n", dockid, current_guess);
                strncpy(shared_address->authStrings[dockid], current_guess, MAX_AUTH_STRING_LEN);

                MessageStruct msg;
                msg.mtype = 3;
                msg.dockId = dockid;
                msg.shipId = shipId;
                msg.direction = direction;

                if (msgsnd(main_msg_queue, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
                    perror("Unable to send undocking message");
                } else {
                    dock_status[dockid] = 0;
                    readyToUndock[dockid] = 0;
                    memset(&ships_in_docks[dockid], 0, sizeof(ShipRequest));
                    lastHandledCargoTs[dockid] = -1;
                    found = 1;
                }
            }
            return;
        }

        for (int i = 0; i < charset_len; i++) {
            current_guess[pos] = charset[i];
            generate_and_try(pos + 1);
            if (found) return;
        }
    }

    generate_and_try(0);

    if (!found) {
        printf("Auth string guess for Dock %d was unsuccessful\n", dockid);
    }
}

//LOADING AND UNLOADING PROCESS
process_docks_for_loading(main_msgq_id,dock_recent,n,dock_cranes);

//UNDOCKING PROCESS (FREQUENCY GUESSING) 
for (int i = 0; i < n; i++) {
        if (readyToUndock[i]) {
            undocking(i, main_msgq_id, shared_memory, m);
            readyToUndock[i] = 0;
            dock_recent[i] = 0;
        }
    }
    
//UPDATING TIMESTEP
void updateTs(int msg_queue){
    MessageStruct increment_ts_message;
    increment_ts_message.mtype = 5;
    if (msgsnd(msg_queue, (void *)&increment_ts_message, sizeof(increment_ts_message) - sizeof(increment_ts_message.mtype), 0) == -1) {
        perror("Error sending message to Validation");
        exit(1);
    }
}
     updateTs(main_msgq_id);

 }
}
