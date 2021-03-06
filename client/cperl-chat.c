#include <cperl-chat.h>

struct cp_win_manage cw_manage[CP_MAX_WIN];
static int term_y = 0, term_x = 0;
int sock;
pthread_t rcv_pthread, info_win_pthread, local_info_win_pthread;
int usr_state;
char time_buf[TIME_BUFFER_SIZE];
char id[ID_SIZE];
char srvname[SERVER_NAME_SIZE];
char plugin_cmd[MESSAGE_BUFFER_SIZE];

struct cp_chat_options options[] = {
    {CP_OPT_HELP, "help", 4, "/help [no argument]: Show CPerl-Chat help messages"},
    {CP_OPT_CONNECT, "connect", 7, "/connect [server name]: Try connect to server"},
    {CP_OPT_DISCONNECT, "disconnect", 10, "/disconnect [no argument]: Try disconnect from server"},
    {CP_OPT_SCRIPT, "script", 6, "/script [script name]: Excute script you made to plugin"},
    {CP_OPT_CLEAR, "clear", 5, "/clear [no argument]: Clear messages in show window"},
    {CP_OPT_REFRESH, "refresh", 6, "F5 Key terminal UI refresh"},
    {CP_OPT_EXIT, "exit", 4, "/exit [no argument]: Exit program"},
    {CP_OPT_LINE, "line", 4, "/line [no argument]: default line 100"}
};

pthread_mutex_t msg_list_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t usr_list_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t info_list_lock = PTHREAD_MUTEX_INITIALIZER;

int scroll_index;
unsigned int msg_count;
unsigned int usr_count;
unsigned int info_count;

struct list_head msg_list;
struct list_head info_list;
struct list_head usr_list[USER_HASH_SIZE];

int main(int argc, char *argv[])
{
    int thr_id;

    // Locale 환경 셋팅
    set_env();
    // Ncurses 환경 초기화
    initscr();
    raw();
    start_color();
    use_default_colors();
    // cperl-chat init
    cp_init_chat();
    // 첫 실행 화면
    first_scr_ui();
    // CPerl-Chat 윈도우 생성
    clear();
    refresh();

    cp_log("start cperl-chat version %s", cp_version);

    cp_create_win();

    line_count = DEFAULT_MSG_COUNT;

    thr_id = pthread_create(&info_win_pthread, NULL, info_win_thread, NULL);
    if(thr_id < 0) {
        cp_log_ui(MSG_ERROR_STATE, "info pthread_create error(%d)", thr_id);
        return -1;
    }
    thr_id = pthread_create(&local_info_win_pthread, NULL, local_info_win_thread, NULL);
    if(thr_id < 0) {
        cp_log_ui(MSG_ERROR_STATE, "local_info pthread_create error(%d)", thr_id);
        return -1;
    }

    if(cp_connect_server(MSG_NEWCONNECT_STATE) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "failed connect server: %s", srvname);
    }

    while(1) {
        char str[MESSAGE_BUFFER_SIZE], *built_str;
        int str_size = 0, built_len = 0;

        str[0] = '\0';
        cw_manage[CP_CHAT_WIN].update_handler();
        get_input_buffer(str);

        if(!(str_size = strlen(str))) 
            continue;

        built_str = msg_build(str, str_size);
        if(!built_str){
            continue;
        }
        built_len = strlen(built_str);

        if(str[0] == OPTION_CHAR) {
            parse_option(built_str);
        } else {
            if(usr_state == USER_LOGIN_STATE) {
                cp_send_data(MSG_DATA_STATE, id, built_str, built_len); 
            }
        }

        free(built_str);
    }

    return 0;
}

void cp_log_print(int type, const char* err_msg, ...)
{
    cp_va_format(err_msg);

    if(vbuffer) {
        insert_msg_list(type, "", "%s%s", "-->", vbuffer);
        cw_manage[CP_SHOW_WIN].update_handler();

        free(vbuffer);
    }
}

int cp_connect_server(int try_type)
{
    struct sockaddr_in srv_addr;
    struct hostent *entry;
    char *resolved_host;
    int thr_id, ret;

    if(try_type == MSG_RECONNECT_STATE) {
        if(sock)
            close(sock);
    }

    clear_usr_list();
    cw_manage[CP_ULIST_WIN].update_handler();

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        cp_log_ui(MSG_ERROR_STATE,  "error: sock(%d)", sock);
        return -1;
    }

    if(cp_sock_option() < 0) {
        return -1;
    }

    entry = gethostbyname(srvname);
    if(!entry) {
        cp_log_ui(MSG_ERROR_STATE, "failed host lookup: srv(%s), check your server name!", srvname);
        return -1;
    } else {
        resolved_host = inet_ntoa((struct in_addr) *((struct in_addr *) entry->h_addr_list[0]));
    }

    memset(&srv_addr, 0x0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr(resolved_host);
    srv_addr.sin_port = htons(atoi(SERVER_PORT));
    if(connect(sock, (struct sockaddr *) &srv_addr, sizeof(srv_addr)) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "%s: errno(%d), srvname(%s), port(%s)", 
                strerror(errno), errno, srvname, SERVER_PORT);
        close(sock);
        return -1;
    }

    // 메시지를 받는 역할을 하는 쓰레드 생성
    if(try_type != MSG_RECONNECT_STATE) {
        thr_id = pthread_create(&rcv_pthread, NULL, rcv_thread, (void *)&sock);
        if(thr_id < 0) {
            cp_log_ui(MSG_ERROR_STATE, "rcv pthread_create error: %d", thr_id);
            close(sock);
            return -1;
        }
    }

    // TCP 접속이 완료되고 서버에게 새로운 사용자라는 것을 전달한다.
    // 이때 알리는 동시에 아이디 값을 같이 전달하게 되어 서버에서 사용자 목록에 추가되게 된다(아이디는 이미 위에서 저장됨).
    if((ret = cp_send_data(try_type, id, NULL, 0)) < 0) {
        cp_logout();
    }

    return 0;
}

void *rcv_thread(void *data) {
    int read_len, state, max_fd;
    char *usr_id, *pbuf;
    fd_set readfds, tmpfds;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    max_fd = sock + 1;

    while(1) {
        struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
        char rcv_buffer[1024];

        tmpfds = readfds;
        state = select(max_fd, &tmpfds, 0, 0, &timeout);
        switch(state) {
            case -1:
                cp_log_ui(MSG_ERROR_STATE, "rcv thread select error!: state(%d)", state);
                break;

            case 0:
                cp_send_data(MSG_AVAILTEST_STATE, id, NULL, 0);
                break;

            default:
                if(FD_ISSET(sock, &tmpfds)) {
                    read_len = read(sock, rcv_buffer, 2048);
                    if(read_len == 0) {
                        cp_log_ui(MSG_ERROR_STATE, 
                                "connection closed with server: server(%s), read_len(%d), errno(%d), strerror(%s)", 
                                srvname, read_len, errno, strerror(errno));
                        cp_logout();

                    } else if(read_len < 0) {
                        switch(errno) {
                            case ETIMEDOUT:
                            case ECONNRESET:
                                cp_log_ui(MSG_ERROR_STATE, "%s(%d), try re-connect...!", strerror(errno), errno);
                                if(cp_connect_server(MSG_RECONNECT_STATE) < 0) {
                                    cp_log_ui(MSG_ERROR_STATE, "re-connect fail...");
                                    break;
                                }
                                continue;

                            default:
                                cp_log_ui(MSG_ERROR_STATE, 
                                        "rcv thread read error: server(%s), read_len(%d), errno(%d), strerror(%s)", 
                                        srvname, read_len, errno, strerror(errno));
                                break;
                        }
                        cp_logout();

                    } else {
                        CP_PACKET packet;

                        memcpy(&packet.cp_h, rcv_buffer, sizeof(CP_PACKET_HEADER));
                        packet.data = (char *)malloc(packet.cp_h.dlen + 1);
                        memcpy(packet.data, rcv_buffer + sizeof(CP_PACKET_HEADER), packet.cp_h.dlen);
                        packet.data[packet.cp_h.dlen] = '\0';
                        /*
                        cp_log_ui(MSG_ERROR_STATE, "read_len:%d, ver:%s, state:%d, dlen:%d, data:%s", 
                                read_len, packet.cp_h.version, packet.cp_h.state, packet.cp_h.dlen, packet.data);
                                */

                        cp_rcv_proc(&packet); 

                        free(packet.data);
                    }
                }

                break;
        }
    }
}

WINDOW *create_window(struct win_ui ui)
{
    WINDOW *win;

    win = newwin(ui.lines, ui.cols, ui.start_y, ui.start_x);
    box(win, 0, 0);
    wborder(win, ui.left, ui.right, ui.top, ui.bottom, 
            ui.ltop, ui.rtop, ui.lbottom, ui.rbottom);
    wrefresh(win);

    return win;
}

void set_env()
{
    char *lc;

    lc = getenv("LC_CTYPE");
    if(lc != NULL) {
        setlocale(LC_CTYPE, lc);
    } else if(lc = getenv("LC_ALL")) {
        setlocale(LC_CTYPE, lc);
    } else {
        setlocale(LC_CTYPE, "");
    }
}

void insert_info_list(const char *info, ...)
{
    struct info_list_node *node;

    cp_va_format(info);

    if(vbuffer) {
        node = (struct info_list_node *)malloc(sizeof(struct info_list_node));
        strcpy(node->message, vbuffer);

        pthread_mutex_lock(&info_list_lock);
        list_add(&node->list, &info_list);
        info_count++;
        pthread_mutex_unlock(&info_list_lock);

        free(vbuffer);
    }
}

void clear_info_list()
{
    struct info_list_node *node, *tnode;

    pthread_mutex_lock(&info_list_lock);
    list_for_each_entry_safe(node, tnode, &info_list, list) {
        list_del(&node->list);
        free(node);
        info_count--;
    }
    pthread_mutex_unlock(&info_list_lock);
}

void update_info_win()
{
    WINDOW *win;
    struct win_ui *ui = &cw_manage[CP_INFO_WIN].ui;
    int i = 0, cline = 0, line_max = 0;
    int print_y, print_x;
    struct info_list_node *node, *tnode;

    pthread_mutex_lock(&info_list_lock);
    win = cw_manage[CP_INFO_WIN].win;
    line_max = ui->lines - 1;
    werase(win);
    list_for_each_entry_safe(node, tnode, &info_list, list) {
        print_y = (ui->lines - 2) - i;
        print_x = 1;

        if(++cline >= line_max) {
            list_del(&node->list);
            free(node);
            continue;
        }

        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, print_y, print_x, node->message);
        wattroff(win, COLOR_PAIR(5));
        i++;
    }
    draw_win_ui(win, *ui);
    pthread_mutex_unlock(&info_list_lock);
}

void update_local_info_win()
{
    WINDOW *win = cw_manage[CP_LO_INFO_WIN].win;
    int print_y, print_x;
    struct ifconf ifconf;
    struct ifreq ifreq[MAXINTERFACES];
    int i, if_cnt;
    static int first_calculate = 0;
    unsigned long cpu_tot = 0, cpu_user = 0, cpu_sys = 0, cpu_nice = 0;
    unsigned long net_bytes_in = 0, net_bytes_out = 0;
    static glibtop_cpu bf_cpu;
    static glibtop_netload bf_netload[MAXINTERFACES];
    glibtop_cpu cpu;
    glibtop_mem memory;
    glibtop_netload netload[MAXINTERFACES];

    if(usr_state == USER_LOGOUT_STATE) {
        werase(win);
        draw_win_ui(win, cw_manage[CP_LO_INFO_WIN].ui);
        return;
    }

    print_y = 1;
    print_x = 1;

    werase(win);

    // cpu informaion
    glibtop_get_cpu(&cpu);
    cpu_tot = cpu.total - bf_cpu.total;
    cpu_user = cpu.user - bf_cpu.user;
    cpu_sys = cpu.sys - bf_cpu.sys;
    cpu_nice = cpu.nice - bf_cpu.nice;
    bf_cpu = cpu;
    if(first_calculate) {
        mvwprintw(win, print_y++, print_x, "CPU         : %d %%", 
                (unsigned int)((100 * (cpu_user + cpu_sys + cpu_nice)) / cpu_tot));
    }

    // memory information
    glibtop_get_mem(&memory);
    if(first_calculate) {
        mvwprintw(win, print_y++, print_x, "Memory      : %ld MB / %ld MB", 
                (unsigned long)memory.used/(1024*1024), (unsigned long)memory.total/(1024*1024));
    }
#if 0
    // network information
    ifconf.ifc_buf = (char *) ifreq;
    ifconf.ifc_len = sizeof ifreq;
    if(ioctl(sock, SIOCGIFCONF, &ifconf) == -1) {
        cp_log_ui(MSG_ERROR_STATE, "ioctl error!: -1");
    }
    if_cnt = ifconf.ifc_len / sizeof(ifreq[0]);

    for(i = 0; i < if_cnt; i++) {
        if(strncmp(ifreq[i].ifr_name, "lo", 2)) {
            glibtop_get_netload(&netload[i], ifreq[i].ifr_name);
            net_bytes_in = netload[i].bytes_in - bf_netload[i].bytes_in;
            net_bytes_out = netload[i].bytes_out - bf_netload[i].bytes_out;
            bf_netload[i] = netload[i];
            if(first_calculate) {
                mvwprintw(win, print_y++, print_x, "%s In/Out : %ld Bytes / %ld Bytes", 
                        ifreq[i].ifr_name, (unsigned long)net_bytes_in, (unsigned long)net_bytes_out);
            }
        }
    }
#endif
    first_calculate = 1;

    draw_win_ui(win, cw_manage[CP_LO_INFO_WIN].ui);
}

void insert_msg_list(int msg_type, char *usr_id, const char *msg, ...)
{
    struct msg_list_node *node, *dnode, *tnode;

    cp_va_format(msg);

    if(vbuffer) {
        scroll_index = 0;

        node = (struct msg_list_node *)malloc(sizeof(struct msg_list_node));
        node->type = msg_type;
        current_time();
        strcpy(node->time, time_buf);
        strcpy(node->id, usr_id);
        strcpy(node->message, vbuffer);

        pthread_mutex_lock(&msg_list_lock);
        list_add(&node->list, &msg_list);
        /* full message count, delete the oldest node */
        if(msg_count >= line_count) {
            list_for_each_entry_safe_reverse(dnode, tnode, &msg_list, list) {
                list_del(&dnode->list);
                free(dnode);
                break;
            }
        } else {
            msg_count++;
        }
        pthread_mutex_unlock(&msg_list_lock);

        free(vbuffer);
    }
}

void clear_msg_list()
{
    struct msg_list_node *node, *tnode;

    pthread_mutex_lock(&msg_list_lock);
    list_for_each_entry_safe(node, tnode, &msg_list, list) {
        list_del(&node->list);
        free(node);
        msg_count--;
    }
    pthread_mutex_unlock(&msg_list_lock);
}

void set_scroll_index(int action)
{
    int win_line = cw_manage[CP_SHOW_WIN].ui.lines - 1;

    switch(action) {
        case SCROLL_UP:
            if((msg_count - scroll_index) >= win_line) {
                scroll_index += 3;
            }
            break;

        case SCROLL_DOWN:
            if(scroll_index >= 3) {
                scroll_index -= 3;
            }
            break;

        case SCROLL_PAGE_UP:
            if((msg_count - scroll_index) >= win_line) {
                scroll_index += (win_line / 2);
            }
            break;

        case SCROLL_PAGE_DOWN:
            if(scroll_index >= (win_line / 2)) {
                scroll_index -= (win_line / 2);
            }
            break;

        case SCROLL_NONE:
        default:
            break;
    }
}

void update_show_win()
{
    WINDOW *win;
    struct win_ui *ui = &cw_manage[CP_SHOW_WIN].ui;
    int i = 0, cline = 0, line_max = 0, scrolled = 0;
    int print_y, print_x;
    struct msg_list_node *node, *tnode;

    pthread_mutex_lock(&msg_list_lock);
    win = cw_manage[CP_SHOW_WIN].win;
    line_max = ui->lines - 1;
    werase(win);
    list_for_each_entry_safe(node, tnode, &msg_list, list) {
        if(scrolled++ < scroll_index) {
            continue;
        }

        print_x = 1;
        print_y = (ui->lines - 2) - i;

        if(++cline >= line_max) {
            break;
        }

        switch(node->type) {
            case MSG_DATA_STATE:
                mvwprintw(win, print_y, print_x, node->time);
                wattron(win, COLOR_PAIR(1) | A_BOLD);
                mvwprintw(win, print_y, print_x += strlen(node->time), node->id);
                wattroff(win, COLOR_PAIR(1) | A_BOLD);
                wattron(win, COLOR_PAIR(2));
                mvwprintw(win, print_y, print_x += strlen(node->id), MESSAGE_SEPARATOR);
                wattroff(win, COLOR_PAIR(2));
                wattron(win, COLOR_PAIR(1));
                mvwprintw(win, print_y, print_x += strlen(MESSAGE_SEPARATOR), node->message);
                wattroff(win, COLOR_PAIR(1));
                break;

            case MSG_DELUSER_STATE:
                mvwprintw(win, print_y, print_x, node->time);
                wattron(win, COLOR_PAIR(3) | A_BOLD);
                mvwprintw(win, print_y, print_x += strlen(node->time), node->id);
                wattroff(win, COLOR_PAIR(3) | A_BOLD);
                wattron(win, COLOR_PAIR(3));
                mvwprintw(win, print_y, print_x += strlen(node->id), "님이 퇴장 하셨습니다!");
                wattroff(win, COLOR_PAIR(3));
                break;

            case MSG_NEWUSER_STATE:
                mvwprintw(win, print_y, print_x, node->time);
                wattron(win, COLOR_PAIR(2) | A_BOLD);
                mvwprintw(win, print_y, print_x += strlen(node->time), node->id);
                wattroff(win, COLOR_PAIR(2) | A_BOLD);
                wattron(win, COLOR_PAIR(2));
                mvwprintw(win, print_y, print_x += strlen(node->id), "님이 입장 하셨습니다!");
                wattroff(win, COLOR_PAIR(2));
                break;

            case MSG_ERROR_STATE:
                mvwprintw(win, print_y, print_x, node->time);
                wattron(win, COLOR_PAIR(3));
                mvwprintw(win, print_y, print_x += strlen(node->time), node->message);
                wattroff(win, COLOR_PAIR(3));
                break;

            case MSG_ALAM_STATE:
            default:
                mvwprintw(win, print_y, print_x, node->time);
                wattron(win, COLOR_PAIR(2));
                mvwprintw(win, print_y, print_x += strlen(node->time), node->message);
                wattroff(win, COLOR_PAIR(2));
                break;
        }
        i++;
    }
    draw_win_ui(win, *ui);
    pthread_mutex_unlock(&msg_list_lock);
}

struct usr_list_node *insert_usr_list(char *id)
{
    struct usr_list_node *node;
    int hash = hash_func(id);

    node = (struct usr_list_node *)malloc(sizeof(struct usr_list_node));
    if(!node) {
        return NULL;
    }

    strcpy(node->id, id);

    pthread_mutex_lock(&usr_list_lock);
    list_add(&node->list, &usr_list[hash]);
    usr_count++;
    pthread_mutex_unlock(&usr_list_lock);

    return node;
}

void delete_usr_list(char* id)
{
    struct usr_list_node *pos;
    int hash = hash_func(id);

    pthread_mutex_lock(&usr_list_lock);
    list_for_each_entry(pos, &usr_list[hash], list) {
        if(!strcmp(pos->id, id)) {
            list_del(&pos->list);
            free(pos);
            pthread_mutex_unlock(&usr_list_lock);
            return;
        }
    }
    pthread_mutex_unlock(&usr_list_lock);
}

void clear_usr_list()
{
    struct usr_list_node *node, *tnode;
    int hash_idx;

    pthread_mutex_lock(&usr_list_lock);
    for(hash_idx = 0; hash_idx < USER_HASH_SIZE; hash_idx++) {
        list_for_each_entry_safe(node, tnode, &usr_list[hash_idx], list) {
            list_del(&node->list);
            free(node);
            usr_count--;
        }
    }
    pthread_mutex_unlock(&usr_list_lock);
}

void update_usr_win()
{
    WINDOW *win;
    struct win_ui *ui = &cw_manage[CP_ULIST_WIN].ui;
    int i = 0, cline = 0, line_max = 0, hash_idx;
    int print_y, print_x;
    struct usr_list_node *node, *tnode;

    pthread_mutex_lock(&usr_list_lock);
    win = cw_manage[CP_ULIST_WIN].win;
    line_max = ui->lines;
    werase(win);
    for(hash_idx = 0; hash_idx < USER_HASH_SIZE; hash_idx++) {
        list_for_each_entry_safe(node, tnode, &usr_list[hash_idx], list) {
            print_y = i + 1;
            print_x = 1;

            if(++cline >= line_max) {
                list_del(&node->list);
                free(node);
                continue;
            }
            wattron(win, COLOR_PAIR(4));
            mvwprintw(win, print_y, print_x, node->id);
            wattroff(win, COLOR_PAIR(4));
            i++;
        }
    }
    draw_win_ui(win, *ui);
    pthread_mutex_unlock(&usr_list_lock);
}

void current_time()
{
    time_t timer;
    struct tm *t;
    int hh, mm, ss;
    int len;

    timer = time(NULL);
    t = localtime(&timer);
    hh = t->tm_hour;
    mm = t->tm_min;
    ss = t->tm_sec;

    len = sprintf(time_buf, "[%02d:%02d:%02d]", hh, mm, ss);
    time_buf[len] = '\0';
}

//info window에 정보를 출력.
void *info_win_thread(void *data) 
{
    int fd, rlen, state;
    char buf[MESSAGE_BUFFER_SIZE + 1];
    char *file_name = INFO_PIPE_FILE;
    char *pbuf, *token, *save_buf;
    struct timeval timeout = { .tv_sec = 60, .tv_usec = 0 };

    fd_set readfds, tmpfds;

    //fifo 파일의 존재 확인
    if(!access(file_name, F_OK)) {
        unlink(file_name);
    }

    if(mkfifo(file_name, 0644) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "make pipe file error : %s", file_name);
    }

    if((fd = open(file_name, O_RDWR)) == -1 ) {
        cp_log_ui(MSG_ERROR_STATE, "pipe file open error : %s", file_name);
    }

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    //select fifo 파일변화 추적
    while(1) {
        tmpfds = readfds;
        system(plugin_cmd);
        state = select(fd + 1, &tmpfds, NULL, NULL, &timeout);
        switch(state) {
            case -1:
                cp_log_ui(MSG_ERROR_STATE, "info thread select error!: state(%d)", state);
                break;

            default :
                if(FD_ISSET(fd, &tmpfds)) {
                    memset(buf, 0x00, sizeof(buf));
                    if((rlen = read(fd, buf, MESSAGE_BUFFER_SIZE)) < 0) {
                        cp_log_ui(MSG_ERROR_STATE, "info read error: %d", rlen);
                        break;
                    }

                    for(pbuf = buf; ; pbuf = NULL) {
                        token = strtok_r(pbuf, DELIM, &save_buf);
                        if(token == NULL) {
                            break;
                        }
                        insert_info_list(token);
                        cw_manage[CP_INFO_WIN].update_handler();
                        wrefresh(cw_manage[CP_CHAT_WIN].win);
                        sleep(1);
                    }
                }
        }
    }
}

void *local_info_win_thread(void *data)
{
    glibtop_init();

    while(1) {
        cw_manage[CP_LO_INFO_WIN].update_handler();
        wrefresh(cw_manage[CP_CHAT_WIN].win);
        sleep(1);
    }
}

void update_chat_win()
{
    WINDOW *win = cw_manage[CP_CHAT_WIN].win;

    werase(win);
    draw_win_ui(win, cw_manage[CP_CHAT_WIN].ui);
    wmove(win, 1, 1);
}

void resize_handler(int sig)
{
    int win_idx;
    struct winsize w;

    endwin();
    initscr();
    clear();
    refresh();

    ioctl(0, TIOCGWINSZ, &w);

    term_y = w.ws_row;
    term_x = w.ws_col;

    update_win_ui();

    for(win_idx= 0; win_idx < CP_MAX_WIN; win_idx++) {
        resize_win_ui(
                cw_manage[win_idx].win, 
                cw_manage[win_idx].ui, 
                cw_manage[win_idx].update_handler
                );
    }
}

void update_win_ui()
{
    cw_manage[CP_SHOW_WIN].ui.lines = (int)((term_y * 70)/100) - 3;
    cw_manage[CP_SHOW_WIN].ui.cols = (term_x - 17);
    cw_manage[CP_SHOW_WIN].ui.start_y = (int)((term_y * 30)/100);
    cw_manage[CP_SHOW_WIN].ui.start_x = 16;
    cw_manage[CP_SHOW_WIN].ui.left = '|';
    cw_manage[CP_SHOW_WIN].ui.right= '|';
    cw_manage[CP_SHOW_WIN].ui.top = '-';
    cw_manage[CP_SHOW_WIN].ui.bottom = '-';
    cw_manage[CP_SHOW_WIN].ui.ltop = '+';
    cw_manage[CP_SHOW_WIN].ui.rtop = '+';
    cw_manage[CP_SHOW_WIN].ui.lbottom = '+';
    cw_manage[CP_SHOW_WIN].ui.rbottom = '+';

    cw_manage[CP_INFO_WIN].ui.lines = (int)((term_y * 30)/100);
    cw_manage[CP_INFO_WIN].ui.cols = (term_x - 17)/2;
    cw_manage[CP_INFO_WIN].ui.start_y = 0;
    cw_manage[CP_INFO_WIN].ui.start_x = 16;
    cw_manage[CP_INFO_WIN].ui.left = '|';
    cw_manage[CP_INFO_WIN].ui.right= '|';
    cw_manage[CP_INFO_WIN].ui.top = '-';
    cw_manage[CP_INFO_WIN].ui.bottom = '-';
    cw_manage[CP_INFO_WIN].ui.ltop = '+';
    cw_manage[CP_INFO_WIN].ui.rtop = '+';
    cw_manage[CP_INFO_WIN].ui.lbottom = '+';
    cw_manage[CP_INFO_WIN].ui.rbottom = '+';

    cw_manage[CP_LO_INFO_WIN].ui.lines = (int)((term_y * 30)/100);
    cw_manage[CP_LO_INFO_WIN].ui.cols = (term_x - 17)/2;
    cw_manage[CP_LO_INFO_WIN].ui.start_y = 0;
    cw_manage[CP_LO_INFO_WIN].ui.start_x = 16 + ((term_x - 17)/2);
    cw_manage[CP_LO_INFO_WIN].ui.left = '|';
    cw_manage[CP_LO_INFO_WIN].ui.right= '|';
    cw_manage[CP_LO_INFO_WIN].ui.top = '-';
    cw_manage[CP_LO_INFO_WIN].ui.bottom = '-';
    cw_manage[CP_LO_INFO_WIN].ui.ltop = '+';
    cw_manage[CP_LO_INFO_WIN].ui.rtop = '+';
    cw_manage[CP_LO_INFO_WIN].ui.lbottom = '+';
    cw_manage[CP_LO_INFO_WIN].ui.rbottom = '+';

    cw_manage[CP_ULIST_WIN].ui.lines = (term_y - 4);
    cw_manage[CP_ULIST_WIN].ui.cols = 15;
    cw_manage[CP_ULIST_WIN].ui.start_y = 0;
    cw_manage[CP_ULIST_WIN].ui.start_x = 0;
    cw_manage[CP_ULIST_WIN].ui.left = '|';
    cw_manage[CP_ULIST_WIN].ui.right= '|';
    cw_manage[CP_ULIST_WIN].ui.top = '-';
    cw_manage[CP_ULIST_WIN].ui.bottom = '-';
    cw_manage[CP_ULIST_WIN].ui.ltop = '+';
    cw_manage[CP_ULIST_WIN].ui.rtop = '+';
    cw_manage[CP_ULIST_WIN].ui.lbottom = '+';
    cw_manage[CP_ULIST_WIN].ui.rbottom = '+';

    cw_manage[CP_CHAT_WIN].ui.lines = 3;
    cw_manage[CP_CHAT_WIN].ui.cols = (term_x - 1);
    cw_manage[CP_CHAT_WIN].ui.start_y = (term_y - 3);
    cw_manage[CP_CHAT_WIN].ui.start_x = 0;
    cw_manage[CP_CHAT_WIN].ui.left = '|';
    cw_manage[CP_CHAT_WIN].ui.right= '|';
    cw_manage[CP_CHAT_WIN].ui.top = '-';
    cw_manage[CP_CHAT_WIN].ui.bottom = '-';
    cw_manage[CP_CHAT_WIN].ui.ltop = '+';
    cw_manage[CP_CHAT_WIN].ui.rtop = '+';
    cw_manage[CP_CHAT_WIN].ui.lbottom = '+';
    cw_manage[CP_CHAT_WIN].ui.rbottom = '+';
}

void cp_init_chat()
{
    cp_init_log("/var/log/cperl-chat.log");

    current_time();

    signal(SIGWINCH, resize_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    mousemask(BUTTON1_CLICKED|BUTTON4_PRESSED|BUTTON2_PRESSED, NULL);

    init_pair(1, COLOR_WHITE, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_RED, -1);
    init_pair(4, COLOR_CYAN, -1);
    init_pair(5, COLOR_YELLOW, -1);

    INIT_LIST_HEAD(&msg_list);
    INIT_LIST_HEAD(&info_list);
    init_usr_list();

    // 처음 사용자의 상태를 로그아웃 상태로 셋팅
    usr_state = USER_LOGOUT_STATE;

    // 초기 플러그인 스크립트 명령 라인 생성
    sprintf(plugin_cmd, "%s%s %s", INFO_SCRIPT_PATH, "daum_rank", INFO_PIPE_FILE);

    term_y = LINES;
    term_x = COLS;

    update_win_ui();
    reg_update_win_func();
}

void resize_win_ui(WINDOW *win, struct win_ui ui, cb_update update)
{
    werase(win);
    wresize(win, ui.lines, ui.cols);
    mvwin(win, ui.start_y, ui.start_x);
    update();
}

void draw_win_ui(WINDOW *win, struct win_ui ui)
{
    wborder(win, ui.right, ui.left, ui.top, ui.bottom, 
            ui.ltop, ui.rtop, ui.lbottom, ui.rbottom);
    wrefresh(win);
}

void reg_update_win_func()
{
    cw_manage[CP_CHAT_WIN].update_handler = update_chat_win;
    cw_manage[CP_SHOW_WIN].update_handler = update_show_win;
    cw_manage[CP_INFO_WIN].update_handler = update_info_win;
    cw_manage[CP_LO_INFO_WIN].update_handler = update_local_info_win;
    cw_manage[CP_ULIST_WIN].update_handler = update_usr_win;
}

void first_scr_ui()
{
    char *first_scr = "Enter your id: ";
    char *srv_name_scr = "Server Name: ";
    char *time_msg_scr = "Acess Time:";
    char *current_time_scr = time_buf;

    wattron(stdscr, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(stdscr, term_y/2 - 8, (term_x - strlen(motd_1))/2, motd_1);
    mvwprintw(stdscr, term_y/2 - 7, (term_x - strlen(motd_1))/2, motd_2);
    mvwprintw(stdscr, term_y/2 - 6, (term_x - strlen(motd_1))/2, motd_3);
    mvwprintw(stdscr, term_y/2 - 5, (term_x - strlen(motd_1))/2, motd_4);
    wattroff(stdscr, COLOR_PAIR(1) | A_BOLD);
    wattron(stdscr, COLOR_PAIR(3) | A_BOLD);
    mvwprintw(stdscr, term_y/2 - 4, (term_x - strlen(motd_1))/2, motd_5);
    mvwprintw(stdscr, term_y/2 - 3, (term_x - strlen(motd_1))/2, motd_6);
    wattroff(stdscr, COLOR_PAIR(3)| A_BOLD);
    mvwprintw(stdscr, term_y/2, (term_x - strlen(first_scr))/2, first_scr); 
    mvwprintw(stdscr, term_y/2 + 2, (term_x - strlen(srv_name_scr))/2 - 1, srv_name_scr);
    mvwprintw(stdscr, term_y/2 + 4, (term_x - strlen(srv_name_scr))/2 - 1, time_msg_scr);
    mvwprintw(stdscr, term_y/2 + 4, (term_x - strlen(srv_name_scr))/2 + 12, current_time_scr);

    while(1) {
        wmove(stdscr, term_y/2, ((term_x - strlen(first_scr))/2) + strlen(first_scr) + 1);
        getnstr(id, ID_SIZE);
        if(!strlen(id)) {
            wattron(stdscr, COLOR_PAIR(3));
            mvwprintw(stdscr, term_y/2 + 6, (term_x - strlen(srv_name_scr))/2 - 1, "ID's length sholud be from 1 to 50");
            wattroff(stdscr, COLOR_PAIR(3));
            continue;
        }   
        break;
    }
    wmove(stdscr, term_y/2 + 6, (term_x - strlen(srv_name_scr))/2 - 1);
    wclrtobot(stdscr);
    while(1) {
        wmove(stdscr, term_y/2 + 2, ((term_x - strlen(srv_name_scr))/2 - 1) + strlen(srv_name_scr) +1);
        getnstr(srvname, SERVER_NAME_SIZE);
        if(!strlen(srvname)){
            wattron(stdscr, COLOR_PAIR(3));
            mvwprintw(stdscr, term_y/2 + 6, (term_x - strlen(srv_name_scr))/2 - 1, "Server name's length sholud be more 1");
            wattroff(stdscr, COLOR_PAIR(3));
            continue;
        }
        break;
    }
}

void cp_create_win()
{
    int win_idx;
    for(win_idx = 0; win_idx < CP_MAX_WIN; win_idx++) {
        cw_manage[win_idx].win = create_window(cw_manage[win_idx].ui);
        keypad(cw_manage[win_idx].win, true);
    }
}

int cp_option_check(char *option, option_type type, bool arg)
{
    if(!arg) {
        if((options[type].op_len == strlen(option)) && 
                !strncmp(options[type].op_name, option, options[type].op_len)) {
            return 1;
        }
    } else {
        if(!strncmp(options[type].op_name, option, options[type].op_len)) {
            return 1;
        }
    }

    return 0;
}

void cp_log_ui(int type, char *log, ...)
{
    cp_va_format(log);

    if(vbuffer) {
        cp_log(vbuffer);
        cp_log_print(type, vbuffer);

        free(vbuffer);
    }
}

void cp_logout()
{
    close(sock);
    usr_state = USER_LOGOUT_STATE; 
    clear_usr_list();
    cw_manage[CP_ULIST_WIN].update_handler();
    cp_log_ui(MSG_ERROR_STATE, "log-out : server(%s)", srvname);
    pthread_cancel(rcv_pthread);
}

void cp_exit()
{
    close(sock);
    pthread_cancel(rcv_pthread);
    pthread_cancel(info_win_pthread);
    pthread_cancel(local_info_win_pthread);
    unlink(INFO_PIPE_FILE);
    endwin();
    exit(0);
}

void cp_rcv_proc(CP_PACKET *p)
{
    char *usr_id, *pbuf;
    char tmp[MESSAGE_BUFFER_SIZE];

    if(!p) {
        return;
    }

    switch(p->cp_h.state) {
        case MSG_ALAM_STATE:
        case MSG_DATA_STATE:
            break;

        case MSG_USERLIST_STATE:

            clear_usr_list();

            memcpy(tmp, p->data, p->cp_h.dlen);
            pbuf = tmp;
            cp_log("received user list from server...: user-list(%s)", pbuf);

            while(usr_id = strtok(pbuf, USER_DELIM)) {
                cp_log("insert user...: user-list(%s)", usr_id);
                insert_usr_list(usr_id);
                pbuf = NULL;
            }

            cw_manage[CP_ULIST_WIN].update_handler();
            wrefresh(cw_manage[CP_CHAT_WIN].win);

            if(usr_state != USER_LOGIN_STATE) {
                cp_log_ui(MSG_ALAM_STATE, "cperl-chat connection: server(%s)", srvname);
            }
            usr_state = USER_LOGIN_STATE;
            return;

        case MSG_NEWUSER_STATE:
            if(!exist_usr_list(p->cp_h.id)) {
                insert_usr_list(p->cp_h.id);
            }
            cw_manage[CP_ULIST_WIN].update_handler();
            break;

        case MSG_DELUSER_STATE:
            delete_usr_list(p->cp_h.id);
            cw_manage[CP_ULIST_WIN].update_handler();
            break;
            
        default:
            return;
    }

    insert_msg_list(p->cp_h.state, p->cp_h.id, "%s", p->data);
    cw_manage[CP_SHOW_WIN].update_handler();
    wrefresh(cw_manage[CP_CHAT_WIN].win);
}

void init_usr_list()
{
    int i;

    for(i = 0; i < USER_HASH_SIZE; i++) {
        INIT_LIST_HEAD(&usr_list[i]);
    }
}

struct usr_list_node *exist_usr_list(char *id)
{
    struct usr_list_node *node = NULL;
    unsigned int hash;

    hash = hash_func(id);
    list_for_each_entry(node, &usr_list[hash], list) {
        if(!strcmp(id, node->id)) {
            return node;
        }
    }

    return NULL;
}

int cp_sock_option()
{
#if 0
    /* linger option */
    struct linger ling;
    socklen_t kalen;
    ling.l_onoff = 0;
    ling.l_linger = 0;
    linglen = sizeof(ling);
    if(setsockopt(sock, SOL_SOCKET, SO_LINGER, &ling, linglen) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "setsock error: linger");
        return -1;
    }
#endif

#if 0
    /* rcv timeout */
    struct timeval tv;
    socklen_t tvlen;
    tv.tv_sec = 5;
    tv.tv_usec =0;
    tvlen = sizeof(tv);
    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, tvlen) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "setsockopt error: rcv timeout");
        return -1;
    }
#endif

#if 0
    /* Set the option active */
    int kaopt;
    socklen_t kalen;
    kaopt = 1;
    kalen = sizeof(kaopt);
    if(setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &kaopt, kalen) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "setsockopt error: keepalive");
        close(sock);
        return -1;
    }
#endif

    return 1;
}

void get_input_buffer(char *input_buffer)
{
    
    MEVENT event; /* for mouse event */
    char _c; /* to distinguish 1byte char or multibyte char */
    int ch, /* intager value gotten from user input device */
        cursor = 1, /* cursor x position of input window */
        cursor_end, /* cursor end should be defined by end of input window */
        buf_idx = 0, /* next position of buffer string end is assigned */
        bytes = 0, /* count of multi byte char */
        ch_cnt = 0; /* count of char */

    while(1) {
        char tmp_buffer[MESSAGE_BUFFER_SIZE];
        /*
        char tmp[MESSAGE_BUFFER_SIZE];
        int i, idx = 0, len;

        len = strlen(input_buffer);
        for(i=0; i < len; i++) {
            idx += sprintf(tmp + idx, "%d,", input_buffer[i]);
        }
        cp_log("dump: %s", tmp);
        */

        wrefresh(cw_manage[CP_CHAT_WIN].win);

        cursor_end = cw_manage[CP_CHAT_WIN].ui.cols - 2;
        _c = ch = mvwgetch(cw_manage[CP_CHAT_WIN].win, 1, cursor);

        if(ch == KEY_F(5)) {
            /* refresh terminal */
            resize_handler(0);
            return;

        } else if(ch == KEY_MOUSE) {
            /* input mouse event */
            if(getmouse(&event) == OK) {
                if (event.bstate & BUTTON4_PRESSED) {
                    set_scroll_index(SCROLL_UP);

                } else if (event.bstate & BUTTON2_PRESSED || event.bstate == 0x8000000) {
                    set_scroll_index(SCROLL_DOWN);
                }

                cw_manage[CP_SHOW_WIN].update_handler();
            }

        } else if (ch == '\n') {
            /* input end */
            break;

        } else if(ch == 21) {
            /* Ctrl + u, clear chars from next position of current cursor to all left string */
            ch_cnt = ch_cnt - (cursor - 1);
            strcpy(tmp_buffer, input_buffer + buf_idx);
            strcpy(input_buffer, tmp_buffer);

            cursor = 1;
            buf_idx = 0;

            cw_manage[CP_CHAT_WIN].update_handler();
            mvwaddstr(cw_manage[CP_CHAT_WIN].win, 1, 1, input_buffer);

        } else if(ch == KEY_HOME) {
            cursor = 1;
            buf_idx = 0;

        } else if(ch == KEY_END) {
            cursor = ch_cnt + 1;
            buf_idx = strlen(input_buffer);

        } else if(ch == KEY_NPAGE) {
            /* page down */
            set_scroll_index(SCROLL_PAGE_DOWN);
            cw_manage[CP_SHOW_WIN].update_handler();

        } else if(ch == KEY_PPAGE) {
            /* page up */
            set_scroll_index(SCROLL_PAGE_UP);
            cw_manage[CP_SHOW_WIN].update_handler();

        } else if(ch == KEY_UP) {
            set_scroll_index(SCROLL_UP);
            cw_manage[CP_SHOW_WIN].update_handler();

        } else if(ch == KEY_DOWN) {
            set_scroll_index(SCROLL_DOWN);
            cw_manage[CP_SHOW_WIN].update_handler();

        } else if(ch == KEY_LEFT) {
            if(cursor <= 1 || buf_idx <= 0) {
                /* if cursor or buffer index is zero or minus index, 
                 * init index and cursor */
                buf_idx = 0;
                cursor = 1;
                continue;
            }

            if(input_buffer[buf_idx - 1] < 0) {
                buf_idx -= 3;
                cursor -= 2;

            } else {
                buf_idx -= 1;
                cursor -= 1;
            }

        } else if(ch == KEY_RIGHT) {
            if(cursor > ch_cnt) {
                continue;
            }

            if(input_buffer[buf_idx] < 0) {
                buf_idx += 3;
                cursor += 2;

            } else {
                buf_idx += 1;
                cursor += 1;
            }

        } else if(ch == KEY_BACKSPACE) {
            if(cursor <= 1 || buf_idx <= 0) {
                continue;
            }

            if(input_buffer[buf_idx - 1] < 0) {
                /* delete 3 bytes char */
                strcpy(tmp_buffer, input_buffer + buf_idx);
                buf_idx -= 3;
                strcpy(input_buffer + buf_idx, tmp_buffer);
                cursor -= 2;
                ch_cnt -= 2;

            } else {
                /* delete 1 bytes char */
                strcpy(tmp_buffer, input_buffer + buf_idx);
                buf_idx -= 1;
                strcpy(input_buffer + buf_idx, tmp_buffer);
                cursor -= 1;
                ch_cnt -= 1;
            }

            cw_manage[CP_CHAT_WIN].update_handler();
            mvwaddstr(cw_manage[CP_CHAT_WIN].win, 1, 1, input_buffer);

        } else if(ch == KEY_DC) {
            char *pull_char;
            int buff_len = strlen(input_buffer);

            if(cursor > ch_cnt) {
                continue;
            }

            if(input_buffer[buf_idx] < 0) {
                pull_char = input_buffer + buf_idx + 3;
                ch_cnt -= 2;
                buff_len -=3;

            } else {
                pull_char = input_buffer + buf_idx + 1;
                ch_cnt -= 1;
                buff_len -=1;
            }
            memcpy(input_buffer + buf_idx, pull_char, strlen(pull_char));
            input_buffer[buff_len] = '\0';

            cw_manage[CP_CHAT_WIN].update_handler();
            mvwaddstr(cw_manage[CP_CHAT_WIN].win, 1, 1, input_buffer);

        } else if(ch == 410) {
            /* exception char */
            continue;
        } else {
            if(cursor >= cursor_end) {
                continue;
            }

            /* if _c >= 0, regard for 1byte char or ascii char however if not, it is multi-bytes 
             * this time char should be considered with en/de-coding type. but that is not considered yet. */
            if(_c >= 0) {
                /* string copy after it will be new char */
                strcpy(tmp_buffer, input_buffer + buf_idx);
                /* char inputed store to buffer */
                buf_idx += sprintf(input_buffer + buf_idx, "%c", ch);
                strcat(input_buffer, tmp_buffer);

                cursor += 1;
                ch_cnt += 1;

                cw_manage[CP_CHAT_WIN].update_handler();
                mvwaddstr(cw_manage[CP_CHAT_WIN].win, 1, 1, input_buffer);

            } else { /* handle 3bytes char */
                /* string copy after it will be new char */
                strcpy(tmp_buffer, input_buffer + buf_idx);
                /* char inputed store to buffer */
                buf_idx += sprintf(input_buffer + buf_idx, "%c", ch);
                strcat(input_buffer, tmp_buffer);

                if(++bytes >= 3) {
                    bytes = 0;
                    cursor += 2;
                    ch_cnt += 2;

                    cw_manage[CP_CHAT_WIN].update_handler();
                    mvwaddstr(cw_manage[CP_CHAT_WIN].win, 1, 1, input_buffer);
                }
            }
        }
    }
}

void msg_list_rearrange()
{
    struct msg_list_node *node, *dnode, *tnode;

    pthread_mutex_lock(&msg_list_lock);
    list_for_each_entry_safe_reverse(dnode, tnode, &msg_list, list) {
        if(msg_count <= line_count) {
            break;
        }

        list_del(&dnode->list);
        free(dnode);
        msg_count--;
    }
    pthread_mutex_unlock(&msg_list_lock);
    return;
}

void parse_option(char *buff) 
{
    char *cur_opt;
    char *argv_parse;

    cur_opt = buff + 1;

    if(cp_option_check(cur_opt, CP_OPT_HELP, false)) {
        int i;

        for(i = 0; i < CP_OPT_MAX; i++) {
            insert_msg_list(MSG_ALAM_STATE, "", options[i].op_desc);
        }
        cw_manage[CP_SHOW_WIN].update_handler();
        return;

    } else if(cp_option_check(cur_opt, CP_OPT_CONNECT, true)) {
        // 이미 사용자 로그인 상태이면 접속하지 않기 위한 처리를 함.
        if(usr_state == USER_LOGIN_STATE) {
            cp_log_ui(MSG_ERROR_STATE, "no more connection.., already connected: srv(%s)", srvname);
            cw_manage[CP_SHOW_WIN].update_handler();
            return;
        }

        argv_parse = strtok(buff, EXEC_DELIM);
        argv_parse = strtok(NULL, EXEC_DELIM);
        if(argv_parse) {
            strcpy(srvname, argv_parse);
        }

        // 접속 시도
        if(cp_connect_server(MSG_NEWCONNECT_STATE) < 0) {
            cp_log_ui(MSG_ERROR_STATE, "failed connect server: %s", srvname);
        }
        return;

    } else if(cp_option_check(cur_opt, CP_OPT_DISCONNECT, false)) {
        cp_logout();
        return;

    } else if(cp_option_check(cur_opt, CP_OPT_SCRIPT, true)) {
        char tfile[FILE_NAME_MAX];

        argv_parse = strtok(buff, EXEC_DELIM);
        argv_parse = strtok(NULL, EXEC_DELIM);
        sprintf(tfile, "%s%s", INFO_SCRIPT_PATH, argv_parse);

        if(!access(tfile, R_OK | X_OK)) {
            sprintf(plugin_cmd, "%s %s", tfile, INFO_PIPE_FILE);

        } else {
            cp_log_ui(MSG_ERROR_STATE, "excute script error: %s cannot access!", tfile);
        }
        return;

    } else if(cp_option_check(cur_opt, CP_OPT_CLEAR, false)) {
        // 메시지 출력창에 있는 메시지를 모두 지운다.
        clear_msg_list();
        cw_manage[CP_SHOW_WIN].update_handler();
        return;

    } else if(cp_option_check(cur_opt, CP_OPT_EXIT, false)) {
        cp_exit();
        return;

    } else if(cp_option_check(cur_opt, CP_OPT_LINE, true)) {
        unsigned int tmp_line_count;
        argv_parse = strtok(buff, EXEC_DELIM);
        argv_parse = strtok(NULL, EXEC_DELIM);

        if(argv_parse) {
            tmp_line_count = atoi(argv_parse);

            if(tmp_line_count < MIN_MSG_COUNT || tmp_line_count > MAX_MSG_COUNT) {
                cp_log_ui(MSG_ERROR_STATE,"invalid count, Max msg count : 500, Min msg count : 100");
                return; 
            } else {
                line_count = tmp_line_count; 
                msg_list_rearrange();
                cp_log_ui(MSG_ERROR_STATE,"Change the linelist : %d",line_count);
            } 
        }
        return;

    } else {
        cp_log_ui(MSG_ERROR_STATE, "invalid options: %s", cur_opt);
        return;
    } 
}

char *msg_build(const char *inbuff, const int inbuff_size)
{
    int inpos = 0, outpos = 0, built_buff_len = inbuff_size + 1;
    char *built;

    built = (char *)malloc(built_buff_len);
    if(!built) {
        return NULL;
    }
    
    while((inpos < inbuff_size) || 
            (inbuff[inpos] != '\0')) {

        if(inbuff[inpos] == '%') {
            if(++built_buff_len <= MESSAGE_BUFFER_SIZE) {
                built = (char *)realloc(built, built_buff_len);

                if(!built) {
                    goto out;
                }

            } else {
                goto out;
            }

            built[outpos++] = '%';
            built[outpos] = '%';

        } else {
            built[outpos] = inbuff[inpos];
        }

        inpos++;
        outpos++;
    }

out:
    built[outpos] = '\0';

    return built;
}

int cp_send_data(int type, char *id, char *data, unsigned int data_len)
{
    CP_PACKET_HEADER cph;
    char send_buffer[1024];

    /* make header */
    strcpy(cph.version, cp_version);
    cph.state = type;
    if(id) {
        memcpy(cph.id, id, strlen(id));
        cph.id[strlen(id)] = '\0';
    }
    cph.dlen = data_len;
    memcpy(send_buffer, &cph, sizeof(CP_PACKET_HEADER));

    /* padding payload */
    if(data_len && data) {
        memcpy(send_buffer + sizeof(CP_PACKET_HEADER), data, data_len);
    }

    if(write(sock, send_buffer, sizeof(CP_PACKET_HEADER) + data_len) < 0) {
        cp_log_ui(MSG_ERROR_STATE, "%s: errno(%d), send_type(%d)", strerror(errno), errno, type);
        return -1;
    }

    return 0;
}
