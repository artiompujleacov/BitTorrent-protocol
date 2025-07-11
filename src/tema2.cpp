#include <fstream>
#include <iostream>
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define RESPONSE_SIZE 5

#define TRACKER_INIT_TAG 0
#define TRACKER_REQ_TAG 1
#define TRACKER_RES_TAG 2
#define UPLOAD_REQ_TAG 3
#define UPLOAD_RES_TAG 4

#define SWARM_UPDATE_CODE 0
#define SWARM_REQ_CODE 1
#define DOWNLOAD_SEGMENT_CODE 2
#define FILE_DOWNLOAD_FINISHED_CODE 3
#define ALL_FINISHED_CODE 4

enum client_type { LEECHER, PEER, SEED };

using namespace std;

typedef struct {
  int num_chunks;
  char name[MAX_FILENAME];
  char chunks[MAX_CHUNKS][HASH_SIZE + 1];
} file_struct;

typedef struct {
  int owned_files_num;
  file_struct owned_files[MAX_FILES];
  int needed_files_num;
  char needed_files_names[MAX_FILES][MAX_FILENAME];
} client_data;

typedef struct {
  int rank;
  client_data *data;
} thread_args;

void read_data(int rank, client_data *data) {
  string filename = "in" + to_string(rank) + ".txt";
  ifstream file(filename);

  if (!file.is_open())
    exit(-1);

  file >> data->owned_files_num;
  for (int i = 0; i < data->owned_files_num; i++) {
    file >> data->owned_files[i].name;
    file >> data->owned_files[i].num_chunks;
    for (int j = 0; j < data->owned_files[i].num_chunks; j++)
      file >> data->owned_files[i].chunks[j];
  }
  file >> data->needed_files_num;
  for (int i = 0; i < data->needed_files_num; i++)
    file >> data->needed_files_names[i];

  file.close();
}

int find_file_index(const char *filename, file_struct *files, int files_count) {
  for (int i = 0; i < files_count; i++)
    if (strcmp(files[i].name, filename) == 0)
      return i;
  return -1;
}

void get_swarm_info(const char *filename, vector<int> &swarm_peers,
                    bool request_hashes) {
  int request_code = SWARM_UPDATE_CODE;
  if (request_hashes) {
    request_code = SWARM_REQ_CODE;
  }

  MPI_Send(&request_code, 1, MPI_INT, TRACKER_RANK, TRACKER_REQ_TAG,
           MPI_COMM_WORLD);
  MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TRACKER_REQ_TAG,
           MPI_COMM_WORLD);
  int peers_num = 0;
  MPI_Recv(&peers_num, 1, MPI_INT, TRACKER_RANK, TRACKER_RES_TAG,
           MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  swarm_peers.clear();
  swarm_peers.resize(peers_num);
  for (int i = 0; i < peers_num; i++) {
    MPI_Recv(&swarm_peers[i], 1, MPI_INT, TRACKER_RANK, TRACKER_RES_TAG,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  }
}

void process_swarm_request(int sender, int request_code,
                           file_struct *swarm_files, int swarm_files_count,
                           vector<pair<int, client_type>> *swarm_clients,
                           int *leechers_num) {
  char filename[MAX_FILENAME];
  MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, sender, TRACKER_REQ_TAG,
           MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  int file_index = find_file_index(filename, swarm_files, swarm_files_count);

  int peers_num = swarm_clients[file_index].size() - leechers_num[file_index];
  MPI_Send(&peers_num, 1, MPI_INT, sender, TRACKER_RES_TAG, MPI_COMM_WORLD);
  for (size_t i = 0; i < swarm_clients[file_index].size(); i++) {
    if (swarm_clients[file_index][i].second != LEECHER) {
      MPI_Send(&swarm_clients[file_index][i].first, 1, MPI_INT, sender,
               TRACKER_RES_TAG, MPI_COMM_WORLD);
      peers_num--;
      if (peers_num == 0)
        break;
    }
  }

  if (request_code == SWARM_UPDATE_CODE) {
    for (size_t i = 0; i < swarm_clients[file_index].size(); i++) {
      if (swarm_clients[file_index][i].first == sender &&
          swarm_clients[file_index][i].second == LEECHER) {
        swarm_clients[file_index][i].second = PEER;
        leechers_num[file_index]--;
        break;
      }
    }
  }

  if (request_code == SWARM_REQ_CODE) {
    MPI_Send(&swarm_files[file_index].num_chunks, 1, MPI_INT, sender,
             TRACKER_RES_TAG, MPI_COMM_WORLD);
    for (int i = 0; i < swarm_files[file_index].num_chunks; i++)
      MPI_Send(swarm_files[file_index].chunks[i], HASH_SIZE + 1, MPI_CHAR,
               sender, TRACKER_RES_TAG, MPI_COMM_WORLD);
    swarm_clients[file_index].push_back({sender, LEECHER});
    leechers_num[file_index]++;
  }
}

void file_download_finished(int sender, file_struct *swarm_files,
                            int swarm_files_count,
                            vector<pair<int, client_type>> *swarm_clients) {
  char filename[MAX_FILENAME];
  MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, sender, TRACKER_REQ_TAG,
           MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  int file_index = find_file_index(filename, swarm_files, swarm_files_count);
  for (size_t i = 0; i < swarm_clients[file_index].size(); i++) {
    if (swarm_clients[file_index][i].first == sender) {
      swarm_clients[file_index][i].second = SEED;
      break;
    }
  }
}

void all_files_downloaded(int sender, vector<bool> &clients_finished,
                          bool &all_clients_finished) {
  clients_finished[sender - 1] = true;
  all_clients_finished = true;
  for (size_t i = 0; i < clients_finished.size(); i++) {
    if (!clients_finished[i]) {
      all_clients_finished = false;
      break;
    }
  }
}

void send_files_to_tracker(int rank, client_data *data) {
  MPI_Send(&data->owned_files_num, 1, MPI_INT, TRACKER_RANK, TRACKER_INIT_TAG,
           MPI_COMM_WORLD);
  for (int i = 0; i < data->owned_files_num; i++) {
    MPI_Send(data->owned_files[i].name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK,
             TRACKER_INIT_TAG, MPI_COMM_WORLD);
    MPI_Send(&data->owned_files[i].num_chunks, 1, MPI_INT, TRACKER_RANK,
             TRACKER_INIT_TAG, MPI_COMM_WORLD);
    for (int j = 0; j < data->owned_files[i].num_chunks; j++)
      MPI_Send(data->owned_files[i].chunks[j], HASH_SIZE + 1, MPI_CHAR,
               TRACKER_RANK, TRACKER_INIT_TAG, MPI_COMM_WORLD);
  }

  char ack[RESPONSE_SIZE];
  MPI_Recv(ack, RESPONSE_SIZE, MPI_CHAR, TRACKER_RANK, TRACKER_INIT_TAG,
           MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  if (strcmp(ack, "OK") != 0)
    cerr << "Failed to send files to tracker\n";
}

void save_file(const char chunks[][HASH_SIZE + 1], int num_chunks,
               const std::string &name, int rank) {
  std::string filename = "client" + std::to_string(rank) + "_" + name;
  std::ofstream file(filename);

  if (file.is_open()) {
    for (int i = 0; i < num_chunks; i++)
      file << chunks[i] << std::endl;
    file.close();
  } else {
    cerr << "Failed to open " << filename << endl;
  }
}

bool download_segment(int seg, file_struct &file, vector<int> &swarm_peers,
                      client_data *data, int file_index, int &downloaded_chunks,
                      bool *chunks_received, int rank) {
  int tried_peers = 0;
  bool no_peer_found = false;
  while (!chunks_received[seg]) {
    if (tried_peers == (int)swarm_peers.size()) {
      cerr << "Nu a fost gasit niciun peer pentru segmentul " << seg
           << " al fisierului " << file.name << "\n";
      no_peer_found = true;
      break;
    }
    int chosen = (downloaded_chunks % swarm_peers.size()) + tried_peers;
    if (chosen >= (int)swarm_peers.size())
      chosen -= swarm_peers.size();
    int chosen_peer = swarm_peers[chosen];
    int request_code = DOWNLOAD_SEGMENT_CODE;
    MPI_Send(&request_code, 1, MPI_INT, chosen_peer, UPLOAD_REQ_TAG,
             MPI_COMM_WORLD);
    MPI_Send(file.name, MAX_FILENAME, MPI_CHAR, chosen_peer, UPLOAD_REQ_TAG,
             MPI_COMM_WORLD);
    MPI_Send(&seg, 1, MPI_INT, chosen_peer, UPLOAD_REQ_TAG, MPI_COMM_WORLD);
    char res[RESPONSE_SIZE];
    MPI_Recv(res, RESPONSE_SIZE, MPI_CHAR, chosen_peer, UPLOAD_RES_TAG,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (strcmp(res, "OK") == 0) {
      char hash[HASH_SIZE + 1];
      MPI_Recv(hash, HASH_SIZE + 1, MPI_CHAR, chosen_peer, UPLOAD_RES_TAG,
               MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      if (strcmp(file.chunks[seg], hash) == 0) {
        chunks_received[seg] = true;
        downloaded_chunks++;
        data->owned_files[data->owned_files_num - 1].num_chunks++;
        strcpy(data->owned_files[file_index].chunks[seg], hash);
      } else {
        tried_peers++;
      }
    } else if (strcmp(res, "NACK") == 0) {
      tried_peers++;
    }
  }
  return no_peer_found;
}

void update_swarm_list(int rank, file_struct &file, vector<int> &swarm_peers) {
  get_swarm_info(file.name, swarm_peers, false);
  for (size_t i = 0; i < swarm_peers.size(); i++) {
    if (swarm_peers[i] == rank) {
      swarm_peers.erase(swarm_peers.begin() + i);
      break;
    }
  }
}

void *download_thread_func(void *arg) {
  thread_args *args = (thread_args *)arg;
  client_data *data = args->data;
  int rank = args->rank;
  int request_code;

  for (int i = 0; i < data->needed_files_num; i++) {
    file_struct file;
    vector<int> swarm_peers;
    bool chunks_received[MAX_CHUNKS] = {false};
    data->owned_files_num++;
    int file_index = data->owned_files_num - 1;
    int downloaded_chunks = 0;

    strcpy(file.name, data->needed_files_names[i]);
    strcpy(data->owned_files[file_index].name, file.name);
    get_swarm_info(file.name, swarm_peers, true);
    MPI_Recv(&file.num_chunks, 1, MPI_INT, TRACKER_RANK, TRACKER_RES_TAG,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    for (int j = 0; j < file.num_chunks; j++)
      MPI_Recv(file.chunks[j], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK,
               TRACKER_RES_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    vector<int> segments_to_download;
    for (int j = 0; j < file.num_chunks; j++)
      segments_to_download.push_back(j);

    bool no_peer_found = false;
    while (downloaded_chunks < file.num_chunks) {
      if (no_peer_found) {
        break;
      }
      int seg = segments_to_download[rand() % segments_to_download.size()];
      for (size_t i = 0; i < segments_to_download.size(); i++) {
        if (segments_to_download[i] == seg) {
          segments_to_download.erase(segments_to_download.begin() + i);
          break;
        }
      }
      no_peer_found =
          download_segment(seg, file, swarm_peers, data, file_index,
                           downloaded_chunks, chunks_received, rank);
      if (downloaded_chunks % 10 == 0)
        update_swarm_list(rank, file, swarm_peers);
    }
    int requestCode = FILE_DOWNLOAD_FINISHED_CODE;
    MPI_Send(&requestCode, 1, MPI_INT, TRACKER_RANK, TRACKER_REQ_TAG,
             MPI_COMM_WORLD);
    MPI_Send(file.name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TRACKER_REQ_TAG,
             MPI_COMM_WORLD);
    save_file(data->owned_files[file_index].chunks, file.num_chunks, file.name,
              rank);
  }

  request_code = ALL_FINISHED_CODE;
  MPI_Send(&request_code, 1, MPI_INT, TRACKER_RANK, TRACKER_REQ_TAG,
           MPI_COMM_WORLD);

  return NULL;
}

void *upload_thread_func(void *arg) {
  thread_args *args = (thread_args *)arg;
  client_data *data = args->data;

  bool running = true;
  while (running) {
    int request_code;
    MPI_Status status;
    MPI_Recv(&request_code, 1, MPI_INT, MPI_ANY_SOURCE, UPLOAD_REQ_TAG,
             MPI_COMM_WORLD, &status);
    int sender = status.MPI_SOURCE;

    if (request_code == DOWNLOAD_SEGMENT_CODE) {
      int seg_index;
      char filename[MAX_FILENAME];
      bool have_segment = false;
      MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, sender, UPLOAD_REQ_TAG,
               MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&seg_index, 1, MPI_INT, sender, UPLOAD_REQ_TAG, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
      for (int i = 0; i < data->owned_files_num; i++) {
        if (strcmp(data->owned_files[i].name, filename) == 0) {
          if (data->owned_files[i].chunks[seg_index][0] != '\0') {
            char res[] = "OK";
            MPI_Send(res, (int)strlen(res) + 1, MPI_CHAR, sender,
                     UPLOAD_RES_TAG, MPI_COMM_WORLD);
            MPI_Send(data->owned_files[i].chunks[seg_index], HASH_SIZE + 1,
                     MPI_CHAR, sender, UPLOAD_RES_TAG, MPI_COMM_WORLD);
            have_segment = true;
            break;
          }
        }
      }

      if (!have_segment) {
        char res[] = "NACK";
        MPI_Send(res, (int)strlen(res) + 1, MPI_CHAR, sender, UPLOAD_RES_TAG,
                 MPI_COMM_WORLD);
      }
    } else if (request_code == ALL_FINISHED_CODE) {
      running = false;
    }
  }
  return NULL;
}

void initialize_tracker_data(int num_tasks, file_struct *swarm_files,
                             int &swarm_files_count,
                             vector<pair<int, client_type>> *swarm_peers) {
  for (int rank = 1; rank < num_tasks; rank++) {
    int owned_files_num = 0;
    MPI_Recv(&owned_files_num, 1, MPI_INT, rank, TRACKER_INIT_TAG,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    for (int i = 0; i < owned_files_num; i++) {
      char filename[MAX_FILENAME];
      MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, rank, TRACKER_INIT_TAG,
               MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      int num_chunks = 0;
      MPI_Recv(&num_chunks, 1, MPI_INT, rank, TRACKER_INIT_TAG, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
      swarm_files[swarm_files_count].num_chunks = num_chunks;

      int file_index =
          find_file_index(filename, swarm_files, swarm_files_count);
      if (file_index == -1) {
        file_index = swarm_files_count;
        strcpy(swarm_files[file_index].name, filename);
        swarm_files_count++;
      }
      for (int j = 0; j < num_chunks; j++) {
        char hashes[HASH_SIZE + 1];
        MPI_Recv(hashes, HASH_SIZE + 1, MPI_CHAR, rank, TRACKER_INIT_TAG,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        strcpy(swarm_files[file_index].chunks[j], hashes);
      }
      swarm_peers[file_index].push_back({rank, SEED});
    }
  }
  for (int rank = 1; rank < num_tasks; rank++) {
    char ack[3] = "OK";
    MPI_Send(ack, 3, MPI_CHAR, rank, TRACKER_INIT_TAG, MPI_COMM_WORLD);
  }
}

void tracker(int num_tasks, int rank) {
  int swarm_files_count = 0;
  file_struct swarm_files[MAX_FILES];
  vector<pair<int, client_type>> swarm_clients[MAX_FILES];
  int leechers_num[MAX_FILES] = {0};
  vector<bool> clients_finished(num_tasks - 1, false);

  initialize_tracker_data(num_tasks, swarm_files, swarm_files_count,
                          swarm_clients);

  bool all_clients_finished = false;
  int request_code;
  while (!all_clients_finished) {
    MPI_Status status;
    MPI_Recv(&request_code, 1, MPI_INT, MPI_ANY_SOURCE, TRACKER_REQ_TAG,
             MPI_COMM_WORLD, &status);
    int sender = status.MPI_SOURCE;
    if (request_code == SWARM_UPDATE_CODE || request_code == SWARM_REQ_CODE) {
      process_swarm_request(sender, request_code, swarm_files,
                            swarm_files_count, swarm_clients, leechers_num);
    } else if (request_code == FILE_DOWNLOAD_FINISHED_CODE) {
      file_download_finished(sender, swarm_files, swarm_files_count,
                             swarm_clients);
    } else if (request_code == ALL_FINISHED_CODE) {
      all_files_downloaded(sender, clients_finished, all_clients_finished);
    }
  }
  for (int rank = 1; rank < num_tasks; rank++) {
    request_code = ALL_FINISHED_CODE;
    MPI_Send(&request_code, 1, MPI_INT, rank, UPLOAD_REQ_TAG, MPI_COMM_WORLD);
  }
}

void peer(int num_tasks, int rank) {
  pthread_t download_thread;
  pthread_t upload_thread;
  void *status;
  int r;
  client_data data;
  data.owned_files_num = 0;
  data.needed_files_num = 0;
  for (int i = 0; i < MAX_FILES; i++)
    data.owned_files[i].num_chunks = 0;
  for (int i = 0; i < MAX_FILES; i++)
    for (int j = 0; j < MAX_CHUNKS; j++)
      data.owned_files[i].chunks[j][0] = '\0';

  read_data(rank, &data);
  send_files_to_tracker(rank, &data);
  thread_args threadArgs;
  threadArgs.rank = rank;
  threadArgs.data = &data;
  r = pthread_create(&download_thread, NULL, download_thread_func,
                     (void *)&threadArgs);
  if (r) {
    printf("Eroare la crearea thread-ului de download\n");
    exit(-1);
  }

  r = pthread_create(&upload_thread, NULL, upload_thread_func,
                     (void *)&threadArgs);
  if (r) {
    printf("Eroare la crearea thread-ului de upload\n");
    exit(-1);
  }

  r = pthread_join(download_thread, &status);
  if (r) {
    printf("Eroare la asteptarea thread-ului de download\n");
    exit(-1);
  }

  r = pthread_join(upload_thread, &status);
  if (r) {
    printf("Eroare la asteptarea thread-ului de upload\n");
    exit(-1);
  }
}

int main(int argc, char *argv[]) {
  int num_tasks, rank;
  int provided;

  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
  if (provided < MPI_THREAD_MULTIPLE) {
    fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
    exit(-1);
  }
  MPI_Comm_size(MPI_COMM_WORLD, &num_tasks);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  srand(time(NULL) + rank);

  if (rank == TRACKER_RANK) {
    tracker(num_tasks, rank);
  } else {
    peer(num_tasks, rank);
  }

  MPI_Finalize();
}
