#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <iostream>
#include <system_error>
#include <memory>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>

#include "control-cmds.h"
#include "service-constants.h"
#include "cpbuffer.h"
#include "dinit-client.h"

// dinitctl:  utility to control the Dinit daemon, including starting and stopping of services.

// This utility communicates with the dinit daemon via a unix stream socket (/dev/initctl, or $HOME/.dinitctl).


enum class command_t;

static int issue_load_service(int socknum, const char *service_name, bool find_only = false);
static int check_load_reply(int socknum, cpbuffer_t &, handle_t *handle_p, service_state_t *state_p);
static int start_stop_service(int socknum, cpbuffer_t &, const char *service_name, command_t command,
        bool do_pin, bool wait_for_service, bool verbose);
static int unpin_service(int socknum, cpbuffer_t &, const char *service_name, bool verbose);
static int unload_service(int socknum, cpbuffer_t &, const char *service_name);
static int list_services(int socknum, cpbuffer_t &);
static int shutdown_dinit(int soclknum, cpbuffer_t &);


static const char * describeState(bool stopped)
{
    return stopped ? "stopped" : "started";
}

static const char * describeVerb(bool stop)
{
    return stop ? "stop" : "start";
}

enum class command_t {
    NONE,
    START_SERVICE,
    WAKE_SERVICE,
    STOP_SERVICE,
    RELEASE_SERVICE,
    UNPIN_SERVICE,
    UNLOAD_SERVICE,
    LIST_SERVICES,
    SHUTDOWN
};


// Entry point.
int main(int argc, char **argv)
{
    using namespace std;
    
    bool show_help = argc < 2;
    char *service_name = nullptr;
    
    std::string control_socket_str;
    const char * control_socket_path = nullptr;
    
    bool verbose = true;
    bool sys_dinit = false;  // communicate with system daemon
    bool wait_for_service = true;
    bool do_pin = false;
    
    command_t command = command_t::NONE;
        
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--help") == 0) {
                show_help = true;
                break;
            }
            else if (strcmp(argv[i], "--no-wait") == 0) {
                wait_for_service = false;
            }
            else if (strcmp(argv[i], "--quiet") == 0) {
                verbose = false;
            }
            else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
                sys_dinit = true;
            }
            else if (strcmp(argv[i], "--pin") == 0) {
                do_pin = true;
            }
            else {
                return 1;
            }
        }
        else if (command == command_t::NONE) {
            if (strcmp(argv[i], "start") == 0) {
                command = command_t::START_SERVICE; 
            }
            else if (strcmp(argv[i], "wake") == 0) {
                command = command_t::WAKE_SERVICE;
            }
            else if (strcmp(argv[i], "stop") == 0) {
                command = command_t::STOP_SERVICE;
            }
            else if (strcmp(argv[i], "release") == 0) {
                command = command_t::RELEASE_SERVICE;
            }
            else if (strcmp(argv[i], "unpin") == 0) {
                command = command_t::UNPIN_SERVICE;
            }
            else if (strcmp(argv[i], "unload") == 0) {
                command = command_t::UNLOAD_SERVICE;
            }
            else if (strcmp(argv[i], "list") == 0) {
                command = command_t::LIST_SERVICES;
            }
            else if (strcmp(argv[i], "shutdown") == 0) {
                command = command_t::SHUTDOWN;
            }
            else {
                show_help = true;
                break;
            }
        }
        else {
            // service name
            if (service_name != nullptr) {
                show_help = true;
                break;
            }
            service_name = argv[i];
            // TODO support multiple services
        }
    }
    
    bool no_service_cmd = (command == command_t::LIST_SERVICES || command == command_t::SHUTDOWN);

    if (service_name != nullptr && no_service_cmd) {
        show_help = true;
    }
    
    if ((service_name == nullptr && ! no_service_cmd) || command == command_t::NONE) {
        show_help = true;
    }

    if (show_help) {
        cout << "dinitctl:   control Dinit services" << endl;
        
        cout << "\nUsage:" << endl;
        cout << "    dinitctl [options] start [options] <service-name> : start and activate service" << endl;
        cout << "    dinitctl [options] stop [options] <service-name>  : stop service and cancel explicit activation" << endl;
        cout << "    dinitctl [options] wake [options] <service-name>  : start but do not mark activated" << endl;
        cout << "    dinitctl [options] release [options] <service-name> : release activation, stop if no dependents" << endl;
        cout << "    dinitctl [options] unpin <service-name>           : un-pin the service (after a previous pin)" << endl;
        cout << "    dinitctl unload <service-name>                    : unload the service" << endl;
        cout << "    dinitctl list                                     : list loaded services" << endl;
        cout << "    dinitctl shutdown                                 : stop all services and terminate dinit" << endl;
        
        cout << "\nNote: An activated service continues running when its dependents stop." << endl;
        
        cout << "\nGeneral options:" << endl;
        cout << "  -s, --system     : control system daemon instead of user daemon" << endl;
        cout << "  --quiet          : suppress output (except errors)" << endl;
        
        cout << "\nCommand options:" << endl;
        cout << "  --help           : show this help" << endl;
        cout << "  --no-wait        : don't wait for service startup/shutdown to complete" << endl;
        cout << "  --pin            : pin the service in the requested (started/stopped) state" << endl;
        return 1;
    }
    
    signal(SIGPIPE, SIG_IGN);
    
    control_socket_path = "/dev/dinitctl";
    
    // Locate control socket
    if (! sys_dinit) {
        char * userhome = getenv("HOME");
        if (userhome == nullptr) {
            struct passwd * pwuid_p = getpwuid(getuid());
            if (pwuid_p != nullptr) {
                userhome = pwuid_p->pw_dir;
            }
        }
        
        if (userhome != nullptr) {
            control_socket_str = userhome;
            control_socket_str += "/.dinitctl";
            control_socket_path = control_socket_str.c_str();
        }
        else {
            cerr << "Cannot locate user home directory (set HOME or check /etc/passwd file)" << endl;
            return 1;
        }
    }
    
    int socknum = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socknum == -1) {
        perror("dinitctl: socket");
        return 1;
    }

    struct sockaddr_un * name;
    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + strlen(control_socket_path) + 1;
    name = (struct sockaddr_un *) malloc(sockaddr_size);
    if (name == nullptr) {
        cerr << "dinitctl: Out of memory" << endl;
        return 1;
    }
    
    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, control_socket_path);
    
    int connr = connect(socknum, (struct sockaddr *) name, sockaddr_size);
    if (connr == -1) {
        perror("dinitctl: connect");
        return 1;
    }
    
    try {
        // Start by querying protocol version:
        cpbuffer_t rbuffer;
        check_protocol_version(0, 0, rbuffer, socknum);

        if (command == command_t::UNPIN_SERVICE) {
            return unpin_service(socknum, rbuffer, service_name, verbose);
        }
        else if (command == command_t::UNLOAD_SERVICE) {
            return unload_service(socknum, rbuffer, service_name);
        }
        else if (command == command_t::LIST_SERVICES) {
            return list_services(socknum, rbuffer);
        }
        else if (command == command_t::SHUTDOWN) {
            return shutdown_dinit(socknum, rbuffer);
        }
        else {
            return start_stop_service(socknum, rbuffer, service_name, command, do_pin,
                    wait_for_service, verbose);
        }
    }
    catch (cp_old_client_exception &e) {
        std::cerr << "dinitctl: too old (server reports newer protocol version)" << std::endl;
        return 1;
    }
    catch (cp_old_server_exception &e) {
        std::cerr << "dinitctl: server too old or protocol error" << std::endl;
        return 1;
    }
    catch (cp_read_exception &e) {
        cerr << "dinitctl: control socket read failure or protocol error" << endl;
        return 1;
    }
    catch (cp_write_exception &e) {
        cerr << "dinitctl: control socket write error: " << std::strerror(e.errcode) << endl;
        return 1;
    }
}

// Start/stop a service
static int start_stop_service(int socknum, cpbuffer_t &rbuffer, const char *service_name,
        command_t command, bool do_pin, bool wait_for_service, bool verbose)
{
    using namespace std;

    bool do_stop = (command == command_t::STOP_SERVICE || command == command_t::RELEASE_SERVICE);
    
    if (issue_load_service(socknum, service_name)) {
        return 1;
    }

    // Now we expect a reply:
    
    wait_for_reply(rbuffer, socknum);

    service_state_t state;
    //service_state_t target_state;
    handle_t handle;

    if (check_load_reply(socknum, rbuffer, &handle, &state) != 0) {
        return 0;
    }

    service_state_t wanted_state = do_stop ? service_state_t::STOPPED : service_state_t::STARTED;
    int pcommand = 0;
    switch (command) {
    case command_t::STOP_SERVICE:
        pcommand = DINIT_CP_STOPSERVICE;
        break;
    case command_t::RELEASE_SERVICE:
        pcommand = DINIT_CP_RELEASESERVICE;
        break;
    case command_t::START_SERVICE:
        pcommand = DINIT_CP_STARTSERVICE;
        break;
    case command_t::WAKE_SERVICE:
        pcommand = DINIT_CP_WAKESERVICE;
        break;
    default: ;
    }

    // Need to issue STOPSERVICE/STARTSERVICE
    // We'll do this regardless of the current service state / target state, since issuing
    // start/stop also sets or clears the "explicitly started" flag on the service.
    {
        int r;
        
        {
            auto buf = new char[2 + sizeof(handle)];
            unique_ptr<char[]> ubuf(buf);

            buf[0] = pcommand;
            buf[1] = do_pin ? 1 : 0;
            memcpy(buf + 2, &handle, sizeof(handle));
            r = write_all(socknum, buf, 2 + sizeof(handle));
        }
        
        if (r == -1) {
            perror("dinitctl: write");
            return 1;
        }
        
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_ALREADYSS) {
            bool already = (state == wanted_state);
            if (verbose) {
                cout << "Service " << (already ? "(already) " : "") << describeState(do_stop) << "." << endl;
            }
            return 0; // success!
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: Protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (! wait_for_service) {
        if (verbose) {
            cout << "Issued " << describeVerb(do_stop) << " command successfully." << endl;
        }
        return 0;
    }

    service_event_t completionEvent;
    service_event_t cancelledEvent;

    if (do_stop) {
        completionEvent = service_event_t::STOPPED;
        cancelledEvent = service_event_t::STOPCANCELLED;
    }
    else {
        completionEvent = service_event_t::STARTED;
        cancelledEvent = service_event_t::STARTCANCELLED;
    }

    // Wait until service started:
    int r = rbuffer.fill_to(socknum, 2);
    while (r > 0) {
        if (rbuffer[0] >= 100) {
            int pktlen = (unsigned char) rbuffer[1];
            fill_buffer_to(rbuffer, socknum, pktlen);

            if (rbuffer[0] == DINIT_IP_SERVICEEVENT) {
                handle_t ev_handle;
                rbuffer.extract((char *) &ev_handle, 2, sizeof(ev_handle));
                service_event_t event = static_cast<service_event_t>(rbuffer[2 + sizeof(ev_handle)]);
                if (ev_handle == handle) {
                    if (event == completionEvent) {
                        if (verbose) {
                            cout << "Service " << describeState(do_stop) << "." << endl;
                        }
                        return 0;
                    }
                    else if (event == cancelledEvent) {
                        if (verbose) {
                            cout << "Service " << describeVerb(do_stop) << " cancelled." << endl;
                        }
                        return 1;
                    }
                    else if (! do_stop && event == service_event_t::FAILEDSTART) {
                        if (verbose) {
                            cout << "Service failed to start." << endl;
                        }
                        return 1;
                    }
                }
            }

            rbuffer.consume(pktlen);
            r = rbuffer.fill_to(socknum, 2);
        }
        else {
            // Not an information packet?
            cerr << "dinitctl: protocol error" << endl;
            return 1;
        }
    }

    if (r == -1) {
        perror("dinitctl: read");
    }
    else {
        cerr << "protocol error (connection closed by server)" << endl;
    }
    return 1;
}

// Issue a "load service" command (DINIT_CP_LOADSERVICE), without waiting for
// a response. Returns 1 on failure (with error logged), 0 on success.
static int issue_load_service(int socknum, const char *service_name, bool find_only)
{
    // Build buffer;
    uint16_t sname_len = strlen(service_name);
    int bufsize = 3 + sname_len;
    int r;
    
    try {
        std::unique_ptr<char[]> ubuf(new char[bufsize]);
        auto buf = ubuf.get();
        
        buf[0] = find_only ? DINIT_CP_FINDSERVICE : DINIT_CP_LOADSERVICE;
        memcpy(buf + 1, &sname_len, 2);
        memcpy(buf + 3, service_name, sname_len);
        
        r = write_all(socknum, buf, bufsize);
    }
    catch (std::bad_alloc &badalloc) {
        std::cerr << "dinitctl: " << badalloc.what() << std::endl;
        return 1;
    }
    
    if (r == -1) {
        perror("dinitctl: write");
        return 1;
    }
    
    return 0;
}

// Check that a "load service" reply was received, and that the requested service was found.
static int check_load_reply(int socknum, cpbuffer_t &rbuffer, handle_t *handle_p, service_state_t *state_p)
{
    using namespace std;
    
    if (rbuffer[0] == DINIT_RP_SERVICERECORD) {
        fill_buffer_to(rbuffer, socknum, 2 + sizeof(*handle_p));
        rbuffer.extract((char *) handle_p, 2, sizeof(*handle_p));
        if (state_p) *state_p = static_cast<service_state_t>(rbuffer[1]);
        //target_state = static_cast<service_state_t>(rbuffer[2 + sizeof(handle)]);
        rbuffer.consume(3 + sizeof(*handle_p));
        return 0;
    }
    else if (rbuffer[0] == DINIT_RP_NOSERVICE) {
        cerr << "dinitctl: failed to find/load service." << endl;
        return 1;
    }
    else {
        cerr << "dinitctl: protocol error." << endl;
        return 1;
    }
}

static int unpin_service(int socknum, cpbuffer_t &rbuffer, const char *service_name, bool verbose)
{
    using namespace std;
    
    // Build buffer;
    if (issue_load_service(socknum, service_name) == 1) {
        return 1;
    }

    // Now we expect a reply:
    
    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue UNPIN command.
    {
        int r;
        
        {
            char *buf = new char[1 + sizeof(handle)];
            unique_ptr<char[]> ubuf(buf);
            buf[0] = DINIT_CP_UNPINSERVICE;
            memcpy(buf + 1, &handle, sizeof(handle));
            r = write_all(socknum, buf, 2 + sizeof(handle));
        }
        
        if (r == -1) {
            perror("dinitctl: write");
            return 1;
        }
        
        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    if (verbose) {
        cout << "Service unpinned." << endl;
    }
    return 0;
}

static int unload_service(int socknum, cpbuffer_t &rbuffer, const char *service_name)
{
    using namespace std;

    // Build buffer;
    if (issue_load_service(socknum, service_name, true) == 1) {
        return 1;
    }

    // Now we expect a reply:

    wait_for_reply(rbuffer, socknum);

    handle_t handle;

    if (check_load_reply(socknum, rbuffer, &handle, nullptr) != 0) {
        return 1;
    }

    // Issue UNLOAD command.
    {
        int r;

        {
            char *buf = new char[1 + sizeof(handle)];
            unique_ptr<char[]> ubuf(buf);
            buf[0] = DINIT_CP_UNLOADSERVICE;
            memcpy(buf + 1, &handle, sizeof(handle));
            r = write_all(socknum, buf, 2 + sizeof(handle));
        }

        if (r == -1) {
            perror("dinitctl: write");
            return 1;
        }

        wait_for_reply(rbuffer, socknum);
        if (rbuffer[0] == DINIT_RP_NAK) {
            cerr << "dinitctl: Could not unload service; service not stopped, or is a dependency of "
                    "other service." << endl;
            return 1;
        }
        if (rbuffer[0] != DINIT_RP_ACK) {
            cerr << "dinitctl: Protocol error." << endl;
            return 1;
        }
        rbuffer.consume(1);
    }

    cout << "Service unloaded." << endl;
    return 0;
}

static int list_services(int socknum, cpbuffer_t &rbuffer)
{
    using namespace std;
    
    char cmdbuf[] = { (char)DINIT_CP_LISTSERVICES };
    int r = write_all(socknum, cmdbuf, 1);

    if (r == -1) {
        perror("dinitctl: write");
        return 1;
    }

    wait_for_reply(rbuffer, socknum);
    while (rbuffer[0] == DINIT_RP_SVCINFO) {
        fill_buffer_to(rbuffer, socknum, 8);
        int nameLen = rbuffer[1];
        service_state_t current = static_cast<service_state_t>(rbuffer[2]);
        service_state_t target = static_cast<service_state_t>(rbuffer[3]);

        fill_buffer_to(rbuffer, socknum, nameLen + 8);

        char *name_ptr = rbuffer.get_ptr(8);
        int clength = std::min(rbuffer.get_contiguous_length(name_ptr), nameLen);

        string name = string(name_ptr, clength);
        name.append(rbuffer.get_buf_base(), nameLen - clength);

        cout << "[";

        cout << (target  == service_state_t::STARTED ? "{" : " ");
        cout << (current == service_state_t::STARTED ? "+" : " ");
        cout << (target  == service_state_t::STARTED ? "}" : " ");
        
        if (current == service_state_t::STARTING) {
            cout << "<<";
        }
        else if (current == service_state_t::STOPPING) {
            cout << ">>";
        }
        else {
            cout << "  ";
        }
        
        cout << (target  == service_state_t::STOPPED ? "{" : " ");
        cout << (current == service_state_t::STOPPED ? "-" : " ");
        cout << (target  == service_state_t::STOPPED ? "}" : " ");

        cout << "] " << name << endl;

        rbuffer.consume(8 + nameLen);
        wait_for_reply(rbuffer, socknum);
    }

    if (rbuffer[0] != DINIT_RP_LISTDONE) {
        cerr << "dinitctl: Control socket protocol error" << endl;
        return 1;
    }

    return 0;
}

static int shutdown_dinit(int socknum, cpbuffer_t &rbuffer)
{
    // TODO support no-wait option.
    using namespace std;

    // Build buffer;
    constexpr int bufsize = 2;
    char buf[bufsize];

    buf[0] = DINIT_CP_SHUTDOWN;
    buf[1] = static_cast<char>(shutdown_type_t::HALT);

    int r = write_all(socknum, buf, bufsize);
    if (r == -1) {
        perror("write");
        return 1;
    }

    wait_for_reply(rbuffer, socknum);

    if (rbuffer[0] != DINIT_RP_ACK) {
        cerr << "dinitctl: Control socket protocol error" << endl;
        return 1;
    }

    // Now wait for rollback complete:
    try {
        while (true) {
            wait_for_info(rbuffer, socknum);
            if (rbuffer[0] == DINIT_ROLLBACK_COMPLETED) {
                break;
            }
        }
    }
    catch (cp_read_exception &exc) {
        // Dinit can terminate before replying: let's assume that happened.
        // TODO: better check, possibly ensure that dinit actually sends rollback complete before
        // termination.
    }

    return 0;
}
