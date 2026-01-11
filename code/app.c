#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#define DEVICE_DS1302   "/dev/ds1302"
#define DEVICE_ROTARY   "/dev/rotary"
#define DEVICE_OLED     "/dev/oled"
#define DEVICE_DHT11    "/dev/dht11"

typedef enum {
    SCREEN_NORMAL,
    SCREEN_TIME_EDIT
} screen_mode_t;

typedef enum {
    EDIT_YEAR = 0,
    EDIT_MONTH,
    EDIT_DAY,
    EDIT_HOUR,
    EDIT_MINUTE,
    EDIT_SECOND,
    EDIT_DONE
} edit_field_t;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} time_data_t;

typedef struct {
    char ds1302_data[64];
    char dht11_data[64];
    int temp;
    int humi;
    screen_mode_t screen_mode;
    edit_field_t edit_field;
    time_data_t edit_time;
    int update_display;
    int running;
} shared_data_t;

shared_data_t shared = {0};
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

int ds1302_fd = -1;
int rotary_fd = -1;
int oled_fd = -1;
int dht11_fd = -1;

pthread_t thread_ds1302, thread_dht11, thread_rotary, thread_oled;

// 필드별 최소/최대값
typedef struct {
    int min;
    int max;
    const char* name;
} field_limit_t;

const field_limit_t field_limits[] = {
    {0, 99, "Year"},    // 2000-2099
    {1, 12, "Month"},
    {1, 31, "Day"},
    {0, 23, "Hour"},
    {0, 59, "Minute"},
    {0, 59, "Second"}
};

void signal_handler(int sig) {
    printf("\n종료 중...\n");
    shared.running = 0;
    
    pthread_cancel(thread_ds1302);
    pthread_cancel(thread_dht11);
    pthread_cancel(thread_rotary);
    pthread_cancel(thread_oled);
    
    if (ds1302_fd >= 0) close(ds1302_fd);
    if (rotary_fd >= 0) close(rotary_fd);
    if (oled_fd >= 0) close(oled_fd);
    if (dht11_fd >= 0) close(dht11_fd);
    
    printf("✓ 종료 완료\n");
    exit(0);
}

// DS1302 문자열 파싱
void parse_ds1302_time(const char* time_str, time_data_t* time_data) {
    // "2025-12-28 14:30:25" 형식
    sscanf(time_str, "%*d-%d-%d %d:%d:%d",
           &time_data->month,
           &time_data->day,
           &time_data->hour,
           &time_data->minute,
           &time_data->second);
    
    int full_year;
    sscanf(time_str, "%d-", &full_year);
    time_data->year = full_year - 2000;
}

// 시간 데이터를 DS1302 형식으로 변환
void apply_time_to_ds1302(time_data_t* time_data) {
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "%02d%02d%02d%02d%02d%02d",
            time_data->year,
            time_data->month,
            time_data->day,
            time_data->hour,
            time_data->minute,
            time_data->second);
    
    write(ds1302_fd, cmd, 12);
    printf("✓ 시간 설정: 20%02d-%02d-%02d %02d:%02d:%02d\n",
           time_data->year, time_data->month, time_data->day,
           time_data->hour, time_data->minute, time_data->second);
}

// Thread 1: DS1302
void* ds1302_thread(void* arg)
{
    char buff[64];
    int ret;
    
    printf("[DS1302] Thread started\n");
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    while (shared.running) {
        memset(buff, 0, sizeof(buff));
        ret = read(ds1302_fd, buff, sizeof(buff) - 1);
        
        if (ret > 0) {
            buff[ret] = '\0';
            char *newline = strchr(buff, '\n');
            if (newline) *newline = '\0';
            
            pthread_mutex_lock(&data_mutex);
            strncpy(shared.ds1302_data, buff, sizeof(shared.ds1302_data) - 1);
            
            if (shared.screen_mode == SCREEN_NORMAL) {
                shared.update_display = 1;
            }
            pthread_mutex_unlock(&data_mutex);
        }
        
        sleep(1);
    }
    
    return NULL;
}

// Thread 2: DHT11
void* dht11_thread(void* arg)
{
    char buf[64];
    int temp, humi;
    int ret;

    printf("[DHT11] Thread started\n");

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    if (dht11_fd < 0) {
        pthread_mutex_lock(&data_mutex);
        shared.temp = -1;
        shared.humi = -1;
        pthread_mutex_unlock(&data_mutex);
    }

    while (shared.running) {
        if (dht11_fd < 0) {
            sleep(2);
            continue;
        }

        memset(buf, 0, sizeof(buf));
        ret = read(dht11_fd, buf, sizeof(buf) - 1);
        
        if (ret > 0) {
            buf[ret] = '\0';
            char *newline = strchr(buf, '\n');
            if (newline) *newline = '\0';

            if (sscanf(buf, "Temp : %d c, Humi : %d", &temp, &humi) == 2 ||
                sscanf(buf, "temp: %d c humi: %d", &temp, &humi) == 2) {
                
                pthread_mutex_lock(&data_mutex);
                shared.temp = temp;
                shared.humi = humi;
                
                if (shared.screen_mode == SCREEN_NORMAL) {
                    shared.update_display = 1;
                }
                pthread_mutex_unlock(&data_mutex);
                
                printf("[DHT11] 온도: %dC, 습도: %d%%\n", temp, humi);
            }
        }

        sleep(3);
    }

    return NULL;
}

// Thread 3: 로터리
void* rotary_thread(void* arg)
{
    char event[16];
    int ret;
    
    printf("[Rotary] Thread started\n");
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    while (shared.running) {
        ret = read(rotary_fd, event, sizeof(event) - 1);
        
        if (ret > 0) {
            event[ret] = '\0';
            char *newline = strchr(event, '\n');
            if (newline) *newline = '\0';
            
            pthread_mutex_lock(&data_mutex);
            
            if (strcmp(event, "CLICK") == 0) {
                if (shared.screen_mode == SCREEN_NORMAL) {
                    // 편집 모드 진입
                    printf("[Rotary] CLICK → 시간 편집 모드 진입\n");
                    
                    // 현재 시간을 편집 버퍼로 복사
                    parse_ds1302_time(shared.ds1302_data, &shared.edit_time);
                    
                    shared.screen_mode = SCREEN_TIME_EDIT;
                    shared.edit_field = EDIT_YEAR;
                    shared.update_display = 1;
                }
                else if (shared.screen_mode == SCREEN_TIME_EDIT) {
                    // 다음 필드로 이동
                    shared.edit_field++;
                    
                    if (shared.edit_field >= EDIT_DONE) {
                        // 편집 완료 → DS1302에 적용
                        printf("[Rotary] CLICK → 시간 보정 완료\n");
                        
                        apply_time_to_ds1302(&shared.edit_time);
                        
                        shared.screen_mode = SCREEN_NORMAL;
                        shared.edit_field = EDIT_YEAR;
                        
                        // 완료 메시지
                        write(oled_fd, "Time Saved!", 11);
                        pthread_mutex_unlock(&data_mutex);
                        sleep(1);
                        pthread_mutex_lock(&data_mutex);
                    }
                    else {
                        printf("[Rotary] CLICK → %s 편집\n", 
                               field_limits[shared.edit_field].name);
                    }
                    
                    shared.update_display = 1;
                }
            }
            else if (strcmp(event, "CW") == 0) {
                if (shared.screen_mode == SCREEN_TIME_EDIT) {
                    // 현재 필드 값 증가
                    int* field_ptr;
                    
                    switch(shared.edit_field) {
                        case EDIT_YEAR:   field_ptr = &shared.edit_time.year;   break;
                        case EDIT_MONTH:  field_ptr = &shared.edit_time.month;  break;
                        case EDIT_DAY:    field_ptr = &shared.edit_time.day;    break;
                        case EDIT_HOUR:   field_ptr = &shared.edit_time.hour;   break;
                        case EDIT_MINUTE: field_ptr = &shared.edit_time.minute; break;
                        case EDIT_SECOND: field_ptr = &shared.edit_time.second; break;
                        default: field_ptr = NULL;
                    }
                    
                    if (field_ptr) {
                        (*field_ptr)++;
                        if (*field_ptr > field_limits[shared.edit_field].max) {
                            *field_ptr = field_limits[shared.edit_field].min;
                        }
                        shared.update_display = 1;
                        printf("[Rotary] CW → %s: %d\n", 
                               field_limits[shared.edit_field].name, *field_ptr);
                    }
                }
            }
            else if (strcmp(event, "CCW") == 0) {
                if (shared.screen_mode == SCREEN_TIME_EDIT) {
                    // 현재 필드 값 감소
                    int* field_ptr;
                    
                    switch(shared.edit_field) {
                        case EDIT_YEAR:   field_ptr = &shared.edit_time.year;   break;
                        case EDIT_MONTH:  field_ptr = &shared.edit_time.month;  break;
                        case EDIT_DAY:    field_ptr = &shared.edit_time.day;    break;
                        case EDIT_HOUR:   field_ptr = &shared.edit_time.hour;   break;
                        case EDIT_MINUTE: field_ptr = &shared.edit_time.minute; break;
                        case EDIT_SECOND: field_ptr = &shared.edit_time.second; break;
                        default: field_ptr = NULL;
                    }
                    
                    if (field_ptr) {
                        (*field_ptr)--;
                        if (*field_ptr < field_limits[shared.edit_field].min) {
                            *field_ptr = field_limits[shared.edit_field].max;
                        }
                        shared.update_display = 1;
                        printf("[Rotary] CCW → %s: %d\n", 
                               field_limits[shared.edit_field].name, *field_ptr);
                    }
                }
            }
            
            pthread_mutex_unlock(&data_mutex);
        }
    }
    
    return NULL;
}

// Thread 4: OLED
void* oled_thread(void* arg)
{
    screen_mode_t last_mode = -1;
    char display_buf[256];
    
    printf("[OLED] Thread started\n");
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    while (shared.running) {
        pthread_mutex_lock(&data_mutex);
        
        if (shared.update_display) {
            
            if (last_mode != shared.screen_mode) {
                write(oled_fd, "CLEAR", 5);
                usleep(50000);
                last_mode = shared.screen_mode;
            }
            
            if (shared.screen_mode == SCREEN_NORMAL) {
                int year, month, day, hour, minute, second;
                
                if (sscanf(shared.ds1302_data, "%d-%d-%d %d:%d:%d",
                          &year, &month, &day, &hour, &minute, &second) == 6) {
                    
                    // 특수 포맷으로 전송
                    // "DATE:2025-12-28\nTIME:14:30:25\nTEMP:25\nHUMI:60"
                    if (shared.temp >= 0 && shared.humi >= 0) {
                        snprintf(display_buf, sizeof(display_buf),
                                "DATE:%04d-%02d-%02d\nTIME:%02d:%02d:%02d\nTEMP:%d\nHUMI:%d",
                                year, month, day,
                                hour, minute, second,
                                shared.temp, shared.humi);
                    } else {
                        snprintf(display_buf, sizeof(display_buf),
                                "DATE:%04d-%02d-%02d\nTIME:%02d:%02d:%02d\nTEMP:--\nHUMI:--",
                                year, month, day,
                                hour, minute, second);
                    }
                } else {
                    snprintf(display_buf, sizeof(display_buf),
                            "DATE:----\nTIME:--:--:--\nTEMP:--\nHUMI:--");
                }
                
                write(oled_fd, display_buf, strlen(display_buf));
                printf("[OLED] Updated\n");
            }
            else if (shared.screen_mode == SCREEN_TIME_EDIT) {
                const char* field_name = field_limits[shared.edit_field].name;
                int field_value;
                
                switch(shared.edit_field) {
                    case EDIT_YEAR:   field_value = shared.edit_time.year;   break;
                    case EDIT_MONTH:  field_value = shared.edit_time.month;  break;
                    case EDIT_DAY:    field_value = shared.edit_time.day;    break;
                    case EDIT_HOUR:   field_value = shared.edit_time.hour;   break;
                    case EDIT_MINUTE: field_value = shared.edit_time.minute; break;
                    case EDIT_SECOND: field_value = shared.edit_time.second; break;
                    default: field_value = 0;
                }
                
                // 편집 화면
                snprintf(display_buf, sizeof(display_buf),
                        "Edit: %s\n>> %02d <<",
                        field_name,
                        field_value);
                
                write(oled_fd, display_buf, strlen(display_buf));
                printf("[OLED] Edit: %s = %d\n", field_name, field_value);
            }
            
            shared.update_display = 0;
        }
        
        pthread_mutex_unlock(&data_mutex);
        
        usleep(100000);
    }
    
    return NULL;
}

int main(int argc, char *argv[])
{
    printf("=== Smart Clock with Time Edit ===\n");
    
    signal(SIGINT, signal_handler);
    
    ds1302_fd = open(DEVICE_DS1302, O_RDWR);
    if (ds1302_fd < 0) {
        perror("open DS1302");
        return -1;
    }
    printf("✓ DS1302 opened\n");
    
    rotary_fd = open(DEVICE_ROTARY, O_RDONLY);
    if (rotary_fd < 0) {
        perror("open Rotary");
        close(ds1302_fd);
        return -1;
    }
    printf("✓ Rotary opened\n");
    
    oled_fd = open(DEVICE_OLED, O_RDWR);
    if (oled_fd < 0) {
        perror("open OLED");
        close(ds1302_fd);
        close(rotary_fd);
        return -1;
    }
    printf("✓ OLED opened\n");
    
    dht11_fd = open(DEVICE_DHT11, O_RDONLY);
    if (dht11_fd < 0) {
        printf("⚠ DHT11 not available\n");
    } else {
        printf("✓ DHT11 opened\n");
    }
    
    if (argc > 1) {
        if (strlen(argv[1]) == 12) {
            printf("초기 시간 설정: %s\n", argv[1]);
            write(ds1302_fd, argv[1], 12);
            sleep(1);
        }
    }
    
    shared.screen_mode = SCREEN_NORMAL;
    shared.update_display = 1;
    shared.running = 1;
    shared.temp = -1;
    shared.humi = -1;
    
    pthread_create(&thread_ds1302, NULL, ds1302_thread, NULL);
    pthread_create(&thread_dht11, NULL, dht11_thread, NULL);
    pthread_create(&thread_rotary, NULL, rotary_thread, NULL);
    pthread_create(&thread_oled, NULL, oled_thread, NULL);
    
    printf("\n========================================\n");
    printf("  스마트 시계 실행 중!\n");
    printf("  - 클릭: 시간 편집 모드\n");
    printf("  - 편집 모드에서 회전: 값 변경\n");
    printf("  - 편집 모드에서 클릭: 다음 필드\n");
    printf("  - Ctrl+C: 종료\n");
    printf("========================================\n\n");
    
    pthread_join(thread_ds1302, NULL);
    pthread_join(thread_dht11, NULL);
    pthread_join(thread_rotary, NULL);
    pthread_join(thread_oled, NULL);
    
    close(ds1302_fd);
    close(rotary_fd);
    close(oled_fd);
    if (dht11_fd >= 0) close(dht11_fd);
    
    return 0;
}
