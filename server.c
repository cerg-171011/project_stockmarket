#include <stdio.h>
#include <microhttpd.h>
#include <cjson/cJSON.h>
#include <sqlite3.h>
#include <libwebsockets.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
 what do u what do - 
// TODO не забыть освободить память для username и password в handle_login и handle_register


#define MAX_VALUES 200
#define TIMESTAMP_SIZE 60
#define MAX_USERS 1000

unsigned int counter = 0;

static int websocket_callback(struct lws* , enum lws_callback_reasons, void* , void* , size_t );
struct Client* INIT_CLIENT();
struct websocket_list_data* INIT_WEBSOCKET_INIT_DATA();


struct websocket_list_data{
    struct lws* wsi_mass[MAX_USERS];
    int index_mass[MAX_USERS];
    bool ready_to_send_mass[MAX_USERS];
    int count_client;
};

struct ConnectData{
    char buffer[1024];
    size_t size;
    struct data_of_cookie* data_cookie;
    struct lws* wsi;
};

static struct lws_protocols protocols[] = {
    {
        "my-protocol",          // Имя протокола
        websocket_callback,     // Функция обратного вызова
        sizeof(struct ConnectData), // Размер пользовательских данных
        0                       // Максимальный размер буфера (можно оставить 0)
    },
    { NULL, NULL, 0, 0 }        // Завершающая запись
};

pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ws_list_mutex = PTHREAD_MUTEX_INITIALIZER;


struct memory {
    char *response;
    size_t size;
};

struct data_API{
    double value[MAX_VALUES];
    char timestamp[MAX_VALUES][TIMESTAMP_SIZE];
    int count_value;
    bool flag_send_to_user;
};

struct data_of_cookie{
    char time_cookie[60];
    char cookie_id[33];
    bool flag_first_attemp;    
};

struct data_API data_response = {{0}, {0}, 0, false};
struct websocket_list_data* ws_list_global = NULL;
struct websocket_list_data* INIT_WEBSOCKET_INIT_DATA(){
    struct websocket_list_data* data = (struct websocket_list_data*) malloc(sizeof(struct websocket_list_data));
    if(data == NULL) return NULL;
    for(int i = 0; i < MAX_USERS; i++){
        data->index_mass[i] = i;
        data->ready_to_send_mass[i] = false;
        data->wsi_mass[i] = NULL;
    } 
    data->count_client = 0;
    return data;
}

void show_all_headers(struct MHD_Connection* connection){
   const char *headers[] = {
        "Host", "User-Agent", "Accept", "Accept-Encoding", "Connection", "Cookie", "Content-Type", NULL
    };

    // Перебираем заголовки
    for (int i = 0; headers[i] != NULL; i++) {
        const char *header_value = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, headers[i]);
        if (header_value != NULL) {
            printf("%s: %s\n", headers[i], header_value);
        } else {
            printf("%s: not found\n", headers[i]);
        }
    }
}

char* get_query_params(const char* data, const char* param) {
    // Находим начало параметра
    const char* key_position = strstr(data, param);
    if (key_position == NULL) {
        fprintf(stderr, "Cannot find parameter \"%s\" in query\n", param);
        return NULL;
    }

    // Смещаем указатель на символ '='
    key_position += strlen(param);
    if (*key_position != '=') {
        fprintf(stderr, "Invalid format for parameter \"%s\" in query\n", param);
        return NULL;
    }
    key_position++;

    // Определяем конец параметра (символ '&' или конец строки)
    const char* end = strstr(key_position, "&");
    size_t length = end ? (size_t)(end - key_position) : strlen(key_position);

    // Выделяем память и копируем строку
    char* result = malloc(length + 1);
    if (result == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }
    strncpy(result, key_position, length);
    result[length] = '\0'; // Обязательно завершаем строку нулевым символом

    return result;
}

int create_table_in_DB(sqlite3* db, const char* name_table, const char* params){
    char create_table[200] = {0};
    int rc = 0;
    snprintf(create_table, sizeof(create_table), "CREATE TABLE IF NOT EXISTS %s (%s)", name_table, params);
    
    rc = sqlite3_exec(db, create_table, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s, Table: %s\n", sqlite3_errmsg(db), name_table);
        sqlite3_close(db);
        return 1;
    }
    return 0;
}

char* get_current_time_string() {
    char* time_now = malloc(60); 
    if (!time_now) {
        fprintf(stderr, "Failed to allocate memory\n");
        return NULL;
    }
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(time_now, 60, "%Y-%m-%d %H:%M:%S", tm_info);
    return time_now;
}

char* get_session_id(){
    char* session_id = malloc(33);
    if(!session_id)
        return MHD_NO;
    
    snprintf(session_id, 33, "%lx%lx", time(NULL), rand());
    printf("Generated NEW session_ID = %s\n", session_id);
    return session_id;
}

int add_or_update_cookie_in_db(sqlite3* db, const struct data_of_cookie* struct_cookie){
    if(struct_cookie->flag_first_attemp == true){
        const char* sql = "INSERT INTO session_table (session_id, session_time) VALUES (?, ?);";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL); 
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
            return 0;
        }

        sqlite3_bind_text(stmt, 1, struct_cookie->cookie_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, struct_cookie->time_cookie, -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "1Failed to execute statement: %s\n", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_finalize(stmt);
        return 1;
    }
    else{
        const char* sql = "UPDATE session_table SET session_time = ? WHERE session_id = ?;";

        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);  // Подготавливаем SQL-запрос
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
            return 0;
        }

        // Привязываем значения к placeholder'ам в SQL-запросе
        sqlite3_bind_text(stmt, 1, struct_cookie->time_cookie, -1, SQLITE_STATIC);  // Привязываем новое время
        sqlite3_bind_text(stmt, 2, struct_cookie->cookie_id, -1, SQLITE_STATIC);  // Привязываем cookie_id

        // Выполняем запрос
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "2Failed to execute statement: %s\n", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);  // Освобождаем ресурсы
            return 0;
        }

    // Завершаем работу с подготовленным запросом
        sqlite3_finalize(stmt);
        return 1;
    }  
}

int copy_cookie_into_data(struct ConnectData* data, bool flag_first){
    char* time_temp = NULL;
    char* cookie_id_temp = NULL;
    data->data_cookie = (struct data_of_cookie*) malloc(sizeof(struct data_of_cookie));
    if(data->data_cookie == NULL)
        return 0;
    memset(data->data_cookie, 0, sizeof(struct data_of_cookie));
    data->data_cookie->flag_first_attemp = flag_first == true ? true : false;
    if(flag_first){
        strncpy(data->data_cookie->cookie_id, (cookie_id_temp = get_session_id()), sizeof(data->data_cookie->cookie_id));
        free(cookie_id_temp);
    }
    strncpy(data->data_cookie->time_cookie, (time_temp = get_current_time_string()), sizeof(data->data_cookie->time_cookie));
    free(time_temp);
    return 1;
}

enum MHD_Result send_response(struct MHD_Connection* connection, const char* response_msg, struct data_of_cookie* data, sqlite3* db){
    char* response_msg_temp = malloc(strlen(response_msg) + 1);
    if(response_msg_temp == NULL) 
        return MHD_NO;
    strncpy(response_msg_temp, response_msg, strlen(response_msg));
    response_msg_temp[strlen(response_msg)] = '\0';
    
    struct MHD_Response* response = MHD_create_response_from_buffer(strlen(response_msg_temp), (void*) response_msg_temp, MHD_RESPMEM_PERSISTENT);
    sqlite3_close(db);
    
    if(data && data->flag_first_attemp == true){
        char sql_zapros[100];
        snprintf(sql_zapros, sizeof(sql_zapros), "session_id=%s", data->cookie_id);
        MHD_add_response_header(response, "Set-Cookie", sql_zapros);
        MHD_add_response_header(response, "Content-Type", "text/html");
        MHD_add_response_header(response, "Connection", "keep-alive");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type");
        int rc = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return rc;
    }
    else{
        MHD_add_response_header(response, "Connection", "keep-alive");
        MHD_add_response_header(response, "Content-Type", "text/html");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type");
        int rc = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return rc;
    }
}

int get_param_from_db(sqlite3* db, char* table, char* key, char* value) {
    char sql[200] = {0};
    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE %s=?", key, table, key);

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    rc = sqlite3_bind_text(stmt, 1, value, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        // Значение найдено
        sqlite3_finalize(stmt);
        return 1;
    } else if (rc == SQLITE_DONE) {
        // Значение не найдено
        sqlite3_finalize(stmt);
        return 0;
    } else {
        // Ошибка выполнения
        sqlite3_finalize(stmt);
        return -1;
    }
}

int get_param_from_db_mass(sqlite3* db, char* table, char* key, char* value[], int len_value) {
    if (db == NULL || table == NULL || value == NULL || len_value < 2) {
        fprintf(stderr, "Invalid input parameters.\n");
        return -1;
    }

    char sql[200] = {0};
    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE username = ? AND password = ?;", key, table);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    for(int i = 0; i < len_value; i++){
        rc = sqlite3_bind_text(stmt, i+1, value[i], -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to bind username: %s\n", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return 1;
    } else if (rc == SQLITE_DONE) {
        printf("No matching records found.\n");
        sqlite3_finalize(stmt);
        return 0;
    } else {
        fprintf(stderr, "Error executing statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return -1;
}

int check_user_in_db(char* username, const char* table, sqlite3* db) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT username FROM %s WHERE username = ?;", table);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return -1; // Ошибка при подготовке запроса
    }

    // Привязываем параметр username к запросу
    rc = sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to bind username: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1; // Ошибка привязки параметра
    }

    // Выполняем запрос
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        // Пользователь найден
        sqlite3_finalize(stmt);
        return 1;
    } else if (rc == SQLITE_DONE) {
        // Пользователь не найден
        sqlite3_finalize(stmt);
        return 0;
    } else {
        // Ошибка выполнения запроса
        fprintf(stderr, "Error executing statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
}

int add_user_in_DB(char* username, char* password, char* table, sqlite3* db){
    if(username == NULL || password == NULL)
        return 0;
    
    char sql[200];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (username, password) VALUES (?, ?);", table);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL); 
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "3Failed to execute statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return 1;
}

int save_data_in_buffer(const char* upload_data, size_t* upload_data_size, struct ConnectData* data){
    if(*upload_data_size > 0){
        strncat(data->buffer, upload_data, *upload_data_size);
        data->size += *upload_data_size;
        *upload_data_size = 0;
        return 1;
    }
    return 0;
}

enum MHD_Result handle_login_request(struct MHD_Connection* connection, struct ConnectData* data, sqlite3* db){
    if(data->size > 0){
        char* username = get_query_params(data->buffer, "username");
        char* passwd = get_query_params(data->buffer, "password");
        if(username == NULL || passwd == NULL)
            return MHD_NO;
    
        char resp_msg[100] = {0};
        int result = 0;
        char* values[] = {username, passwd}; 
        if(result = get_param_from_db_mass(db, "register_table", "password", values, 2)){
            snprintf(resp_msg, sizeof(resp_msg), "SUCCSESS AUTH: user: %s in REGISTRATION", username);
            return send_response(connection, resp_msg, data->data_cookie, db);
        }
        else if(result == 0){
            snprintf(resp_msg, sizeof(resp_msg), "FAIL AUTH: No user in DB");
            return send_response(connection, resp_msg, data->data_cookie, db);
        }     
    }
    return MHD_NO;
}

enum MHD_Result handle_resister_request(struct MHD_Connection* connection, struct ConnectData* data, sqlite3* db){
    int result = 0;
    int result1 = 0;

    if(data->size > 0){
        char* username = get_query_params(data->buffer, "username");
        char* passwd = get_query_params(data->buffer, "password");

        result = check_user_in_db(username, "register_table", db);
        result1 = get_param_from_db(db, "register_table", "password", passwd);
        
        if(result == 1 && result1 == 1){
            //printf("User: %s found in DB\n", username);
            return send_response(connection, "SUCCSESS: User already exist", NULL, db);
        }
        else if(result == 0 || result1 == 0){
            if(add_user_in_DB(username, passwd, "register_table", db) == 0)
                return MHD_NO;
            //printf("User: %s add in DB\n", username);
            return send_response(connection, "SUCCSESS: User add in table", NULL, db);
        }
        else{
            return MHD_NO;
        }
    }
    return send_response(connection, "SUCCSESS: Upload data PRINTED", NULL, db);
}

enum MHD_Result return_page(struct MHD_Connection* connection, const char* url, struct ConnectData* data, sqlite3* db) {
    if (strcmp(url, "/") == 0) {
        const char *html_response =
        "<!DOCTYPE html>"
        "<html lang=\"en\">"
        "<head>"
        "  <meta charset=\"UTF-8\">"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "  <title>График цены Bitcoin</title>"
        "  <script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>"
        "  <script src=\"https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns\"></script>"
        "</head>"
        "<body>"
        "  <h1>График цены Bitcoin</h1>"
        "  <canvas id=\"bitcoinChart\" width=\"400\" height=\"200\"></canvas>"
        "  <script>"
        "    const ctx = document.getElementById('bitcoinChart').getContext('2d');"
        "    const data = {"
        "      labels: [],"
        "      datasets: [{"
        "        label: 'Цена Bitcoin (USD)',"
        "        data: [],"
        "        borderColor: 'rgba(75, 192, 192, 1)',"
        "        backgroundColor: 'rgba(75, 192, 192, 0.2)',"
        "        borderWidth: 1,"
        "        fill: true"
        "      }]"
        "    };"
        "    const config = {"
        "      type: 'line',"
        "      data: data,"
        "      options: {"
        "        scales: {"
        "          x: {"
        "            type: 'time',"
        "            position: 'bottom',"
        "            title: {"
        "              display: true,"
        "              text: 'Время'"
        "            },"
        "            time: {"
        "              unit: 'second',"
        "              tooltipFormat: 'll HH:mm:ss'"
        "            }"
        "          },"
        "          y: {"
        "            title: {"
        "              display: true,"
        "              text: 'Цена (USD)'"
        "            },"
        "            ticks: {"
        "              beginAtZero: false"
        "            }"
        "          }"
        "        }"
        "      }"
        "    };"
        "    const bitcoinChart = new Chart(ctx, config);"
        "    const ws = new WebSocket('ws://localhost:8081');"
        "    let lastUpdateTime = Date.now();"
        "    ws.onmessage = (event) => {"
        "      const message = JSON.parse(event.data);"
        "      const timestamp = new Date(message.timestamp).toISOString();"
        "      const value = parseFloat(message.value);"
        "      if (!isFinite(value) || value <= 0) {"
        "        console.error('Некорректное значение:', value);"
        "        return;"
        "      }"
        "      if (Date.now() - lastUpdateTime > 500) {"
        "        lastUpdateTime = Date.now();"
        "        if (data.labels.length >= 50) {"
        "          data.labels.shift();"
        "          data.datasets[0].data.shift();"
        "        }"
        "        data.labels.push(timestamp);"
        "        data.datasets[0].data.push(value);"
        "        bitcoinChart.update();"
        "      }"
        "    };"
        "    ws.onerror = (error) => {"
        "      console.error('WebSocket Error:', error);"
        "    };"
        "  </script>"
        "</body>"
        "</html>";

        return send_response(connection, html_response, data->data_cookie, db);
    }
    return MHD_NO; // Возврат, если URL не соответствует "/"
}


enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection, 
                                            const char *url, const char *method, const char *version, 
                                            const char *upload_data, size_t *upload_data_size, void **ptr){

    if(strcmp("/favicon.ico", url) == 0){
        struct MHD_Response* response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return MHD_YES;
    }

    struct ConnectData* data = *ptr;
    const char *cookie_header = MHD_lookup_connection_value(connection, MHD_COOKIE_KIND, "session_id");

    // Проблема с куками актуальна
    if(cookie_header != NULL)
        printf("COOKIE: %s\n", cookie_header);
    
         
    sqlite3* db = NULL;
    int rc = sqlite3_open("new_db.db", &db);
    if(rc != SQLITE_OK){
        return MHD_NO;
    }
        
    if(data == NULL){
        data = (struct ConnectData*) malloc(sizeof(struct ConnectData));
        if(data == NULL)
            return MHD_NO;
        memset(data,0, sizeof(struct ConnectData));
        *ptr = data;
        
        if(cookie_header == NULL){
            if(copy_cookie_into_data(data, true) == 0)
                return MHD_NO;
            printf("we have: time = %s, cookie_id = %s\n", data->data_cookie->time_cookie, data->data_cookie->cookie_id);
            if(!add_or_update_cookie_in_db(db, data->data_cookie))
                return MHD_NO;
        }
        else{ 
            if(copy_cookie_into_data(data, false) == 0)
                return MHD_NO;
            strncpy(data->data_cookie->cookie_id, cookie_header, sizeof(data->data_cookie->cookie_id));
            if(!add_or_update_cookie_in_db(db, data->data_cookie))
                return MHD_NO;
        }
        return MHD_YES;
    }
    data = *ptr;
    
    if(*upload_data_size > 0){
        if(save_data_in_buffer(upload_data, upload_data_size, data) == 0){
            if(data != NULL)
                free(data);
            return MHD_NO;
        }
        data->buffer[data->size] = '\0';
        return MHD_YES;
    }

    if(strcmp(method, "GET") == 0){
        printf("GET\n");
        if(data != NULL)
            free(data);
        return return_page(connection, url, data, db);
    }

    else if(strcmp(method, "POST") == 0){
        if(strcmp(url, "/login") == 0){
            if(data != NULL)
                free(data);
           return handle_login_request(connection, data, db);
        }
        else if(strcmp(url, "/register") == 0){
            if(data != NULL)
                free(data);
            return handle_resister_request(connection, data, db);
        }
    }
    if(data != NULL)
        free(data);
    sqlite3_close(db);
    return MHD_NO;
}


size_t write_callback(void *ptr, size_t size, size_t nmemb, struct memory *mem) {
    size_t new_len = mem->size + size * nmemb;
    mem->response = realloc(mem->response, new_len + 1);
    if (mem->response == NULL) {
        printf("Error reallocating memory\n");
        return 0;
    }
    memcpy(mem->response + mem->size, ptr, size * nmemb);
    mem->size = new_len;
    mem->response[new_len] = '\0';
    return size * nmemb;
}

void *get_coingecko_data() {
    CURL *curl = NULL;
    CURLcode res;
    cJSON *json = NULL;
    struct memory chunk = {.response = NULL, .size = 0};
    int index = 0;

    while (1) {
        index = 0;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (curl) {
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "accept: application/json");
            headers = curl_slist_append(headers, "x-cg-demo-api-key: CG-Cj6oL7ec2eaEs1Topb5arXyh");

            curl_easy_setopt(curl, CURLOPT_URL, "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl);
            if (res == CURLE_OK && chunk.response != NULL) {
                json = cJSON_Parse(chunk.response);
                if (json != NULL) {
                    cJSON *temp = cJSON_GetObjectItem(json, "bitcoin");
                    if (temp != NULL) {
                        cJSON *val = cJSON_GetObjectItem(temp, "usd");
                        if (val != NULL && cJSON_IsNumber(val)) {
                            unsigned long value = (unsigned long)val->valuedouble;
                            printf("Bitcoin price (USD): %lu\n", value);
                            
                            pthread_mutex_lock(&data_mutex);
                            // Добавление значения в data_response
                            if (data_response.count_value < MAX_VALUES) { 
                                data_response.value[data_response.count_value] = value;
                                strncpy(data_response.timestamp[data_response.count_value], get_current_time_string(), TIMESTAMP_SIZE);
                                counter = data_response.count_value;
                                printf("value = %lf, time = %s\n", data_response.value[counter], data_response.timestamp[counter]);
                                data_response.count_value++;
                                data_response.flag_send_to_user = true;
                            } else {
                                fprintf(stderr, "Warning: data_response storage is full\n");
                            }

                            pthread_mutex_unlock(&data_mutex);
                            pthread_mutex_lock(&ws_list_mutex);
                            for (int i = 0; i < MAX_USERS; i++) {
                                if (ws_list_global->wsi_mass[i] != NULL && ws_list_global->index_mass[i] == -1) {
                                    lws_callback_on_writable(ws_list_global->wsi_mass[i]);
                                    ws_list_global->ready_to_send_mass[i] = true; // Отметим, что данные готовы
                                }
                            }
                            pthread_mutex_unlock(&ws_list_mutex);
                        } else {
                            fprintf(stderr, "Error: 'usd' value is not a number\n");
                        }
                    } else {
                        fprintf(stderr, "Error: 'bitcoin' object not found\n");
                    }
                    cJSON_Delete(json);
                } else {
                    fprintf(stderr, "Error: Failed to parse JSON\n");
                }
            } else {
                fprintf(stderr, "Error: CURL request failed: %s\n", curl_easy_strerror(res));
            }

            free(chunk.response);
            chunk.response = NULL;
            chunk.size = 0;

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();

        Sleep(10000);  
    }
    return NULL;
}

void* start_mhd_server(void* arg){
    
    struct MHD_Daemon* daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, 8080, NULL, NULL, &answer_to_connection, NULL, MHD_OPTION_END);
    if(daemon == NULL){
        fprintf(stderr, "Failed to start HTTP server\n");
        return NULL;
    }

    printf("Start listening on port 8080\n");
    getchar();

    MHD_stop_daemon(daemon);
    return NULL;
}

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    int index = -1;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {  // Клиент подключился
            printf("Client connected\n");

            if (pthread_mutex_lock(&ws_list_mutex) != 0) {
                fprintf(stderr, "Failed to lock ws_list_mutex\n");
                return 1;
            }

            if (ws_list_global->count_client >= MAX_USERS) {
                fprintf(stderr, "Cannot add new users: maximum limit reached\n");
                pthread_mutex_unlock(&ws_list_mutex);
                break;
            }

            // Находим свободный индекс
            for (int i = 0; i < MAX_USERS; i++) {
                if (ws_list_global->index_mass[i] != -1) {
                    index = ws_list_global->index_mass[i];
                    ws_list_global->index_mass[i] = -1;  // Помечаем как занятый
                    ws_list_global->count_client++;
                    break;
                }
            }

            if (index != -1) {
                ws_list_global->wsi_mass[index] = wsi;
                ws_list_global->ready_to_send_mass[index] = true;
                printf("Client added at index %d\n", index);
            } else {
                fprintf(stderr, "No available slots for new clients\n");
            }

            pthread_mutex_unlock(&ws_list_mutex);  // Освобождаем мьютекс
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {  // Сервер готов отправить данные
            printf("Server is ready to send data\n");

            if (pthread_mutex_lock(&ws_list_mutex) != 0 || pthread_mutex_lock(&data_mutex) != 0) {
                fprintf(stderr, "Failed to lock mutexes\n");
                return 1;
            }

            if (data_response.flag_send_to_user == true && data_response.count_value > 0) {
                int value_to_send = data_response.value[data_response.value[counter]];
                unsigned char buf[LWS_PRE + 256];
                unsigned char *p = &buf[LWS_PRE];

                // Формируем JSON строку для отправки
                snprintf((char *)p, sizeof(buf) - LWS_PRE, "{\"value\": %d, \"timestamp\": \"%s\"}", value_to_send, data_response.timestamp[counter]);

                // Отправляем данные всем клиентам
                for (int i = 0; i < MAX_USERS; i++) {
                    if (ws_list_global->wsi_mass[i] == wsi) {
                        if (ws_list_global->index_mass[i] == -1 && ws_list_global->ready_to_send_mass[i]) {
                            printf("Sending data to client at index %d\n", i);
                            lws_write(ws_list_global->wsi_mass[i], p, strlen((char *)p), LWS_WRITE_TEXT);
                            ws_list_global->ready_to_send_mass[i] = false;
                        }
                        break;
                    }
                }

                // Сбрасываем флаг после отправки всем клиентам
                data_response.flag_send_to_user = false;
            } else {
                printf("No data to send\n");
                printf("data_response.flag_send_to_user = %s, data_response.count_value = %d\n",
                       data_response.flag_send_to_user ? "true" : "false", data_response.count_value);
            }

            pthread_mutex_unlock(&data_mutex);
            pthread_mutex_unlock(&ws_list_mutex);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {  // Получены данные от клиента
            printf("Received data: %s\n", (char *)in);

            // Пример отправки ответа клиенту
            unsigned char buf[LWS_PRE + len];
            unsigned char *p = &buf[LWS_PRE];
            memcpy(p, in, len);
            lws_write(wsi, p, len, LWS_WRITE_TEXT);

            break;
        }

        case LWS_CALLBACK_CLOSED: {  // Клиент отключился
            printf("Client disconnected\n");

            if (pthread_mutex_lock(&ws_list_mutex) != 0) {
                fprintf(stderr, "Failed to lock ws_list_mutex\n");
                return 1;
            }

            for (int i = 0; i < MAX_USERS; i++) {
                if (ws_list_global->wsi_mass[i] == wsi) {
                    ws_list_global->wsi_mass[i] = NULL;
                    ws_list_global->count_client--;
                    ws_list_global->index_mass[i] = i;  // Возвращаем индекс в список доступных
                    ws_list_global->ready_to_send_mass[i] = false;
                    printf("Client removed from index %d\n", i);
                    break;
                }
            }

            pthread_mutex_unlock(&ws_list_mutex);
            break;
        }

        default:
            break;
    }

    return 0;
}


void* start_websocket_server(void* arg){
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = 8081; // Порт для WebSocket сервера
    info.protocols = protocols; // Определите протоколы WebSocket
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context* context = lws_create_context(&info);
    if (context == NULL) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        return NULL;
    }

    printf("WebSocket server started on port 8081\n");

    // Главный цикл обработки событий WebSocket
    while (1) {
        lws_service(context, 1000); // Обрабатываем события
    }

    lws_context_destroy(context);
    return NULL;
}

int main(){
    sqlite3* db = NULL;
    ws_list_global = INIT_WEBSOCKET_INIT_DATA();

    int rc = sqlite3_open("new_db.db", &db);
    if(rc != SQLITE_OK){
        return 1;
    }
    create_table_in_DB(db, "session_table", "session_id TEXT, session_time TEXT");
    create_table_in_DB(db, "register_table", "id_user TEXT, username TEXT, password TEXT, email TEXT, last_login_time TEXT, data_registration TEXT, acc_status TEXT");
    sqlite3_close(db);

    pthread_t mhd_thread, websocket_thread, coingecko_api_thread;

    // Запускаем сервер MicroHTTPD в отдельном потоке
    if (pthread_create(&mhd_thread, NULL, start_mhd_server, NULL) != 0) {
        fprintf(stderr, "Failed to create thread for MHD server\n");
        return 1;
    }

    // Запускаем сервер WebSocket в отдельном потоке
    if (pthread_create(&websocket_thread, NULL, start_websocket_server, NULL) != 0) {
        fprintf(stderr, "Failed to create thread for WebSocket server\n");
        return 1;
    }


    if(pthread_create(&coingecko_api_thread, NULL, get_coingecko_data, NULL) != 0){
        fprintf(stderr, "Failed to create thread for Coingecko API\n");
        return 1;
    }

    // Ожидаем завершения потоков
    pthread_join(mhd_thread, NULL);
    pthread_join(websocket_thread, NULL);
    pthread_join(coingecko_api_thread, NULL);

    return 0;
}
