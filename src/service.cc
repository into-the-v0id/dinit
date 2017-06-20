#include <cstring>
#include <cerrno>
#include <sstream>
#include <iterator>
#include <memory>
#include <cstddef>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "service.h"
#include "dinit-log.h"
#include "dinit-socket.h"

/*
 * service.cc - Service management.
 * See service.h for details.
 */

// from dinit.cc:
void open_control_socket(bool report_ro_failure = true) noexcept;
void setup_external_log() noexcept;
extern eventloop_t eventLoop;

// Find the requested service by name
static service_record * find_service(const std::list<service_record *> & records,
                                    const char *name) noexcept
{
    using std::list;
    list<service_record *>::const_iterator i = records.begin();
    for ( ; i != records.end(); i++ ) {
        if (strcmp((*i)->getServiceName().c_str(), name) == 0) {
            return *i;
        }
    }
    return (service_record *)0;
}

service_record * service_set::find_service(const std::string &name) noexcept
{
    return ::find_service(records, name.c_str());
}

void service_set::stopService(const std::string & name) noexcept
{
    service_record *record = find_service(name);
    if (record != nullptr) {
        record->stop();
        process_queues();
    }
}

// Called when a service has actually stopped; dependents have stopped already, unless this stop
// is due to an unexpected process termination.
void service_record::stopped() noexcept
{
    if (onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        discard_console_log_buffer();
        release_console();
    }

    force_stop = false;

    // If we are a soft dependency of another target, break the acquisition from that target now:
    for (auto dependent : soft_dpts) {
        if (dependent->holding_acq) {
            dependent->holding_acq = false;
            release();
        }
    }

    bool will_restart = (desired_state == service_state_t::STARTED)
            && services->get_auto_restart();

    for (auto dependency : depends_on) {
        // we signal dependencies in case they are waiting for us to stop:
        dependency->dependentStopped();
    }

    service_state = service_state_t::STOPPED;

    if (will_restart) {
        // Desired state is "started".
        restarting = true;
        start(false);
    }
    else {
        if (socket_fd != -1) {
            close(socket_fd);
            socket_fd = -1;
        }
        
        if (start_explicit) {
            start_explicit = false;
            release();
        }
        else if (required_by == 0) {
            services->service_inactive(this);
        }
    }

    logServiceStopped(service_name);
    notifyListeners(service_event::STOPPED);
}

dasynq::rearm service_child_watcher::status_change(eventloop_t &loop, pid_t child, int status) noexcept
{
    base_process_service *sr = service;
    
    sr->pid = -1;
    sr->exit_status = status;
    
    // Ok, for a process service, any process death which we didn't rig
    // ourselves is a bit... unexpected. Probably, the child died because
    // we asked it to (sr->service_state == STOPPING). But even if
    // we didn't, there's not much we can do.
    
    if (sr->waiting_for_execstat) {
        // We still don't have an exec() status from the forked child, wait for that
        // before doing any further processing.
        return rearm::REMOVE;
    }
    
    // Must deregister now since handle_exit_status might result in re-launch:
    deregister(loop, child);
    
    sr->handle_exit_status(status);
    return rearm::REMOVED;
}

bool service_record::do_auto_restart() noexcept
{
    if (auto_restart) {
        return services->get_auto_restart();
    }
    return false;
}

void service_record::emergency_stop() noexcept
{
    if (! do_auto_restart() && start_explicit) {
        start_explicit = false;
        release();
    }
    forceStop();
    stopDependents();
    stopped();
}

void process_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);

    if (exit_status != 0 && service_state != service_state_t::STOPPING) {
        if (did_exit) {
            log(LogLevel::ERROR, "Service ", service_name, " process terminated with exit code ", WEXITSTATUS(exit_status));
        }
        else if (was_signalled) {
            log(LogLevel::ERROR, "Service ", service_name, " terminated due to signal ", WTERMSIG(exit_status));
        }
    }

    if (service_state == service_state_t::STARTING) {
        if (did_exit && WEXITSTATUS(exit_status) == 0) {
            started();
        }
        else {
            failed_to_start();
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        stopped();
    }
    else if (smooth_recovery && service_state == service_state_t::STARTED && desired_state == service_state_t::STARTED) {
        // TODO if we are pinned-started then we should probably check
        //      that dependencies have started before trying to re-start the
        //      service process.
        if (! restart_ps_process()) {
            emergency_stop();
            services->process_queues();
        }
        return;
    }
    else {
        emergency_stop();
    }
    services->process_queues();
}

void bgproc_service::handle_exit_status(int exit_status) noexcept
{
    begin:
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);

    if (exit_status != 0 && service_state != service_state_t::STOPPING) {
        if (did_exit) {
            log(LogLevel::ERROR, "Service ", service_name, " process terminated with exit code ", WEXITSTATUS(exit_status));
        }
        else if (was_signalled) {
            log(LogLevel::ERROR, "Service ", service_name, " terminated due to signal ", WTERMSIG(exit_status));
        }
    }

    if (doing_recovery) {
        doing_recovery = false;
        bool need_stop = false;
        if ((did_exit && WEXITSTATUS(exit_status) != 0) || was_signalled) {
            need_stop = true;
        }
        else {
            // We need to re-read the PID, since it has now changed.
            if (pid_file.length() != 0) {
                auto pid_result = read_pid_file(&exit_status);
                switch (pid_result) {
                    case pid_result_t::FAILED:
                        // Failed startup: no auto-restart.
                        need_stop = true;
                        break;
                    case pid_result_t::TERMINATED:
                        goto begin;
                    case pid_result_t::OK:
                        break;
                }
            }
        }

        if (need_stop) {
            // Failed startup: no auto-restart.
            emergency_stop();
            services->process_queues();
        }

        return;
    }

    if (service_state == service_state_t::STARTING) {
        // POSIX requires that if the process exited clearly with a status code of 0,
        // the exit status value will be 0:
        if (exit_status == 0) {
            auto pid_result = read_pid_file(&exit_status);
            switch (pid_result) {
                case pid_result_t::FAILED:
                    // Failed startup: no auto-restart.
                    failed_to_start();
                    break;
                case pid_result_t::TERMINATED:
                    // started, but immediately terminated
                    started();
                    goto begin;
                case pid_result_t::OK:
                    started();
                    break;
            }
        }
        else {
            failed_to_start();
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        // We won't log a non-zero exit status or termination due to signal here -
        // we assume that the process died because we signalled it.
        stopped();
    }
    else if (smooth_recovery && service_state == service_state_t::STARTED && desired_state == service_state_t::STARTED) {
        // TODO if we are pinned-started then we should probably check
        //      that dependencies have started before trying to re-start the
        //      service process.
        doing_recovery = true;
        if (! restart_ps_process()) {
            emergency_stop();
            services->process_queues();
        }
        return;
    }
    else {
        // we must be STARTED
        if (! do_auto_restart() && start_explicit) {
            start_explicit = false;
            release();
        }
        forceStop();
        stopDependents();
        stopped();
    }
    services->process_queues();
}

void scripted_service::handle_exit_status(int exit_status) noexcept
{
    bool did_exit = WIFEXITED(exit_status);
    bool was_signalled = WIFSIGNALED(exit_status);

    if (service_state == service_state_t::STOPPING) {
        if (did_exit && WEXITSTATUS(exit_status) == 0) {
            stopped();
        }
        else {
            // ??? failed to stop! Let's log it as info:
            if (did_exit) {
                log(LogLevel::INFO, "Service ", service_name, " stop command failed with exit code ", WEXITSTATUS(exit_status));
            }
            else if (was_signalled) {
                log(LogLevel::INFO, "Serivice ", service_name, " stop command terminated due to signal ", WTERMSIG(exit_status));
            }
            // Just assume that we stopped, so that any dependencies
            // can be stopped:
            stopped();
        }
        services->process_queues();
    }
    else { // STARTING
        if (exit_status == 0) {
            started();
        }
        else {
            // failed to start
            if (did_exit) {
                log(LogLevel::ERROR, "Service ", service_name, " command failed with exit code ", WEXITSTATUS(exit_status));
            }
            else if (was_signalled) {
                log(LogLevel::ERROR, "Service ", service_name, " command terminated due to signal ", WTERMSIG(exit_status));
            }
            failed_to_start();
        }
        services->process_queues();
    }
}

rearm exec_status_pipe_watcher::fd_event(eventloop_t &loop, int fd, int flags) noexcept
{
    base_process_service *sr = service;
    sr->waiting_for_execstat = false;
    
    int exec_status;
    int r = read(get_watched_fd(), &exec_status, sizeof(int));
    deregister(loop);
    close(get_watched_fd());
    
    if (r > 0) {
        // We read an errno code; exec() failed, and the service startup failed.
        if (sr->pid != -1) {
            sr->child_listener.deregister(eventLoop, sr->pid);
        }
        sr->pid = -1;
        log(LogLevel::ERROR, sr->service_name, ": execution failed: ", strerror(exec_status));
        if (sr->service_state == service_state_t::STARTING) {
            sr->failed_to_start();
        }
        else if (sr->service_state == service_state_t::STOPPING) {
            // Must be a scripted service. We've logged the failure, but it's probably better
            // not to leave the service in STARTED state:
            sr->stopped();
        }
    }
    else {
        // exec() succeeded.
        if (sr->record_type == service_type::PROCESS) {
            // This could be a smooth recovery (state already STARTED). Even more, the process
            // might be stopped (and killed via a signal) during smooth recovery.  We don't to
            // process startup again in either case, so we check for state STARTING:
            if (sr->service_state == service_state_t::STARTING) {
                sr->started();
            }
        }
        
        if (sr->pid == -1) {
            // Somehow the process managed to complete before we even saw the status.
            sr->handle_exit_status(sr->exit_status);
        }
    }
    
    sr->services->process_queues();
    
    return rearm::REMOVED;
}

void service_record::require() noexcept
{
    if (required_by++ == 0) {
        prop_require = !prop_release;
        prop_release = false;
        services->addToPropQueue(this);
    }
}

void service_record::release() noexcept
{
    if (--required_by == 0) {
        desired_state = service_state_t::STOPPED;

        // Can stop, and can release dependencies now. We don't need to issue a release if
        // the require was pending though:
        prop_release = !prop_require;
        prop_require = false;
        services->addToPropQueue(this);

        if (service_state == service_state_t::STOPPED) {
            services->service_inactive(this);
        }
        else {
            do_stop();
        }
    }
}

void service_record::release_dependencies() noexcept
{
    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        (*i)->release();
    }

    for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
        service_record * to = i->getTo();
        if (i->holding_acq) {
            to->release();
            i->holding_acq = false;
        }
    }
}

void service_record::start(bool activate) noexcept
{
    if (activate && ! start_explicit) {
        require();
        start_explicit = true;
    }
    
    if (desired_state == service_state_t::STARTED && service_state != service_state_t::STOPPED) return;

    bool was_active = service_state != service_state_t::STOPPED || desired_state != service_state_t::STOPPED;
    desired_state = service_state_t::STARTED;
    
    if (service_state != service_state_t::STOPPED) {
        // We're already starting/started, or we are stopping and need to wait for
        // that the complete.
        if (service_state != service_state_t::STOPPING || ! can_interrupt_stop()) {
            return;
        }
        // We're STOPPING, and that can be interrupted. Our dependencies might be STOPPING,
        // but if so they are waiting (for us), so they too can be instantly returned to
        // STARTING state.
        notifyListeners(service_event::STOPCANCELLED);
    }
    else if (! was_active) {
        services->service_active(this);
    }

    service_state = service_state_t::STARTING;
    waiting_for_deps = true;

    if (startCheckDependencies(true)) {
        services->addToStartQueue(this);
    }
}

void service_record::do_propagation() noexcept
{
    if (prop_require) {
        // Need to require all our dependencies
        for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
            (*i)->require();
        }

        for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
            service_record * to = i->getTo();
            to->require();
            i->holding_acq = true;
        }
        
        prop_require = false;
    }
    
    if (prop_release) {
        release_dependencies();
        prop_release = false;
    }
    
    if (prop_failure) {
        prop_failure = false;
        failed_to_start(true);
    }
    
    if (prop_start) {
        prop_start = false;
        start(false);
    }

    if (prop_stop) {
        prop_stop = false;
        do_stop();
    }
}

void service_record::execute_transition() noexcept
{
    if (service_state == service_state_t::STARTING) {
        if (startCheckDependencies(false)) {
            allDepsStarted(false);
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        if (stopCheckDependents()) {
            all_deps_stopped();
        }
    }
}

void service_record::do_start() noexcept
{
    if (pinned_stopped) return;
    
    if (service_state != service_state_t::STARTING) {
        return;
    }
    
    service_state = service_state_t::STARTING;

    waiting_for_deps = true;

    // Ask dependencies to start, mark them as being waited on.
    if (startCheckDependencies(false)) {
        // Once all dependencies are started, we start properly:
        allDepsStarted();
    }
}

void service_record::dependencyStarted() noexcept
{
    if (service_state == service_state_t::STARTING && waiting_for_deps) {
        services->addToStartQueue(this);
    }
}

bool service_record::startCheckDependencies(bool start_deps) noexcept
{
    bool all_deps_started = true;

    for (sr_iter i = depends_on.begin(); i != depends_on.end(); ++i) {
        if ((*i)->service_state != service_state_t::STARTED) {
            if (start_deps) {
                all_deps_started = false;
                (*i)->prop_start = true;
                services->addToPropQueue(*i);
            }
            else {
                return false;
            }
        }
    }

    for (auto i = soft_deps.begin(); i != soft_deps.end(); ++i) {
        service_record * to = i->getTo();
        if (start_deps) {
            if (to->service_state != service_state_t::STARTED) {
                to->prop_start = true;
                services->addToPropQueue(to);
                i->waiting_on = true;
                all_deps_started = false;
            }
            else {
                i->waiting_on = false;
            }
        }
        else if (i->waiting_on) {
            if (to->service_state != service_state_t::STARTING) {
                // Service has either started or is no longer starting
                i->waiting_on = false;
            }
            else {
                // We are still waiting on this service
                return false;
            }
        }
    }
    
    return all_deps_started;
}

bool service_record::open_socket() noexcept
{
    if (socket_path.empty() || socket_fd != -1) {
        // No socket, or already open
        return true;
    }
    
    const char * saddrname = socket_path.c_str();
    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + socket_path.length() + 1;

    struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
    if (name == nullptr) {
        log(LogLevel::ERROR, service_name, ": Opening activation socket: out of memory");
        return false;
    }
    
    // Un-link any stale socket. TODO: safety check? should at least confirm the path is a socket.
    unlink(saddrname);

    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, saddrname);

    int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (sockfd == -1) {
        log(LogLevel::ERROR, service_name, ": Error creating activation socket: ", strerror(errno));
        free(name);
        return false;
    }

    if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
        log(LogLevel::ERROR, service_name, ": Error binding activation socket: ", strerror(errno));
        close(sockfd);
        free(name);
        return false;
    }
    
    free(name);
    
    // POSIX (1003.1, 2013) says that fchown and fchmod don't necesarily work on sockets. We have to
    // use chown and chmod instead.
    if (chown(saddrname, socket_uid, socket_gid)) {
        log(LogLevel::ERROR, service_name, ": Error setting activation socket owner/group: ", strerror(errno));
        close(sockfd);
        return false;
    }
    
    if (chmod(saddrname, socket_perms) == -1) {
        log(LogLevel::ERROR, service_name, ": Error setting activation socket permissions: ", strerror(errno));
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 128) == -1) { // 128 "seems reasonable".
        log(LogLevel::ERROR, ": Error listening on activation socket: ", strerror(errno));
        close(sockfd);
        return false;
    }
    
    socket_fd = sockfd;
    return true;
}

void service_record::allDepsStarted(bool has_console) noexcept
{
    if (onstart_flags.starts_on_console && ! has_console) {
        waiting_for_deps = true;
        queue_for_console();
        return;
    }
    
    waiting_for_deps = false;

    // We overload can_interrupt_start to check whether there is any other
    // process (eg restart timer) that needs to finish before starting.
    if (can_interrupt_start()) {
        waiting_for_deps = true;
        return;
    }

    if (! open_socket()) {
        failed_to_start();
    }

    bool start_success = start_ps_process();
    if (! start_success) {
        failed_to_start();
    }
}

void service_record::acquiredConsole() noexcept
{
    if (service_state != service_state_t::STARTING) {
        // We got the console but no longer want it.
        release_console();
    }
    else if (startCheckDependencies(false)) {
        allDepsStarted(true);
    }
    else {
        // We got the console but can't use it yet.
        release_console();
    }
}

bgproc_service::pid_result_t
bgproc_service::read_pid_file(int *exit_status) noexcept
{
    const char *pid_file_c = pid_file.c_str();
    int fd = open(pid_file_c, O_CLOEXEC);
    if (fd == -1) {
        log(LogLevel::ERROR, service_name, ": read pid file: ", strerror(errno));
        return pid_result_t::FAILED;
    }

    char pidbuf[21]; // just enough to hold any 64-bit integer
    int r = read(fd, pidbuf, 20); // TODO signal-safe read
    if (r < 0) {
        // Could not read from PID file
        log(LogLevel::ERROR, service_name, ": could not read from pidfile; ", strerror(errno));
        close(fd);
        return pid_result_t::FAILED;
    }

    close(fd);
    pidbuf[r] = 0; // store nul terminator
    // TODO may need stoull; what if int isn't big enough...
    pid = std::atoi(pidbuf);
    pid_t wait_r = waitpid(pid, exit_status, WNOHANG);
    if (wait_r == -1 && errno == ECHILD) {
        // We can't track this child - check process exists:
        if (kill(pid, 0) == 0) {
            tracking_child = false;
            return pid_result_t::OK;
        }
        else {
            log(LogLevel::ERROR, service_name, ": pid read from pidfile (", pid, ") is not valid");
            pid = -1;
            return pid_result_t::FAILED;
        }
    }
    else if (wait_r == pid) {
        pid = -1;
        return pid_result_t::TERMINATED;
    }
    else if (wait_r == 0) {
        // We can track the child
        // TODO we must use a preallocated watch!!
        child_listener.add_watch(eventLoop, pid);
        tracking_child = true;
        return pid_result_t::OK;
    }
    else {
        log(LogLevel::ERROR, service_name, ": pid read from pidfile (", pid, ") is not valid");
        pid = -1;
        return pid_result_t::FAILED;
    }
}

void service_record::started() noexcept
{
    if (onstart_flags.starts_on_console && ! onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        release_console();
    }

    logServiceStarted(service_name);
    service_state = service_state_t::STARTED;
    notifyListeners(service_event::STARTED);

    if (onstart_flags.rw_ready) {
        open_control_socket();
    }
    if (onstart_flags.log_ready) {
        setup_external_log();
    }

    if (force_stop || desired_state == service_state_t::STOPPED) {
        // We must now stop.
        do_stop();
        return;
    }

    // Notify any dependents whose desired state is STARTED:
    for (auto i = dependents.begin(); i != dependents.end(); i++) {
        (*i)->dependencyStarted();
    }
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        (*i)->getFrom()->dependencyStarted();
    }
}

void service_record::failed_to_start(bool depfailed) noexcept
{
    if (!depfailed && onstart_flags.starts_on_console) {
        tcsetpgrp(0, getpgrp());
        release_console();
    }
    
    logServiceFailed(service_name);
    service_state = service_state_t::STOPPED;
    if (start_explicit) {
        start_explicit = false;
        release();
    }
    notifyListeners(service_event::FAILEDSTART);
    
    // Cancel start of dependents:
    for (sr_iter i = dependents.begin(); i != dependents.end(); i++) {
        if ((*i)->service_state == service_state_t::STARTING) {
            (*i)->prop_failure = true;
            services->addToPropQueue(*i);
        }
    }    
    for (auto i = soft_dpts.begin(); i != soft_dpts.end(); i++) {
        // We can send 'start', because this is only a soft dependency.
        // Our startup failure means that they don't have to wait for us.
        if ((*i)->waiting_on) {
            (*i)->holding_acq = false;
            (*i)->waiting_on = false;
            (*i)->getFrom()->dependencyStarted();
            release();
        }
    }
}

bool service_record::start_ps_process() noexcept
{
    // default implementation: there is no process, so we are started.
    started();
    return true;
}

bool base_process_service::start_ps_process() noexcept
{
    if (restarting) {
        return restart_ps_process();
    }
    else {
        eventLoop.get_time(restart_interval_time, clock_type::MONOTONIC);
        restart_interval_count = 0;
        return start_ps_process(exec_arg_parts, onstart_flags.starts_on_console);
    }
}

bool base_process_service::start_ps_process(const std::vector<const char *> &cmd, bool on_console) noexcept
{
    // In general, you can't tell whether fork/exec is successful. We use a pipe to communicate
    // success/failure from the child to the parent. The pipe is set CLOEXEC so a successful
    // exec closes the pipe, and the parent sees EOF. If the exec is unsuccessful, the errno
    // is written to the pipe, and the parent can read it.

    eventLoop.get_time(last_start_time, clock_type::MONOTONIC);

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) {
        log(LogLevel::ERROR, service_name, ": can't create status check pipe: ", strerror(errno));
        return false;
    }

    const char * logfile = this->logfile.c_str();
    if (*logfile == 0) {
        logfile = "/dev/null";
    }

    bool child_status_registered = false;
    control_conn_t *control_conn = nullptr;
    
    int control_socket[2] = {-1, -1};
    if (onstart_flags.pass_cs_fd) {
        if (dinit_socketpair(AF_UNIX, SOCK_STREAM, /* protocol */ 0, control_socket, SOCK_NONBLOCK)) {
            log(LogLevel::ERROR, service_name, ": can't create control socket: ", strerror(errno));
            goto out_p;
        }
        
        // Make the server side socket close-on-exec:
        int fdflags = fcntl(control_socket[0], F_GETFD);
        fcntl(control_socket[0], F_SETFD, fdflags | FD_CLOEXEC);
        
        try {
            control_conn = new control_conn_t(&eventLoop, services, control_socket[0]);
        }
        catch (std::exception &exc) {
            log(LogLevel::ERROR, service_name, ": can't launch process; out of memory");
            goto out_cs;
        }
    }
    
    // Set up complete, now fork and exec:
    
    pid_t forkpid;
    
    try {
        // We add the status listener with a high priority (i.e. low priority value) so that process
        // termination is handled early. This means we have always recorded that the process is
        // terminated by the time that we handle events that might otherwise cause us to signal the
        // process, so we avoid sending a signal to an invalid (and possibly recycled) process ID.
        child_status_listener.add_watch(eventLoop, pipefd[0], IN_EVENTS, true, DEFAULT_PRIORITY - 10);
        child_status_registered = true;
        
        forkpid = child_listener.fork(eventLoop);
    }
    catch (std::exception &e) {
        log(LogLevel::ERROR, service_name, ": Could not fork: ", e.what());
        goto out_cs_h;
    }

    if (forkpid == 0) {
        run_child_proc(cmd.data(), logfile, on_console, pipefd[1], control_socket[1]);
    }
    else {
        // Parent process
        close(pipefd[1]); // close the 'other end' fd
        if (control_socket[1] != -1) {
            close(control_socket[1]);
        }
        pid = forkpid;

        waiting_for_execstat = true;
        return true;
    }

    // Failure exit:
    
    out_cs_h:
    if (child_status_registered) {
        child_status_listener.deregister(eventLoop);
    }
    
    if (onstart_flags.pass_cs_fd) {
        delete control_conn;
    
        out_cs:
        close(control_socket[0]);
        close(control_socket[1]);
    }
    
    out_p:
    close(pipefd[0]);
    close(pipefd[1]);
    
    return false;
}

void service_record::run_child_proc(const char * const *args, const char *logfile, bool on_console,
        int wpipefd, int csfd) noexcept
{
    // Child process. Must not allocate memory (or otherwise risk throwing any exception)
    // from here until exit().

    // If the console already has a session leader, presumably it is us. On the other hand
    // if it has no session leader, and we don't create one, then control inputs such as
    // ^C will have no effect.
    bool do_set_ctty = (tcgetsid(0) == -1);
    
    // Copy signal mask, but unmask signals that we masked on startup. For the moment, we'll
    // also block all signals, since apparently dup() can be interrupted (!!! really, POSIX??).
    sigset_t sigwait_set;
    sigset_t sigall_set;
    sigfillset(&sigall_set);
    sigprocmask(SIG_SETMASK, &sigall_set, &sigwait_set);
    sigdelset(&sigwait_set, SIGCHLD);
    sigdelset(&sigwait_set, SIGINT);
    sigdelset(&sigwait_set, SIGTERM);
    sigdelset(&sigwait_set, SIGQUIT);
    
    constexpr int bufsz = ((CHAR_BIT * sizeof(pid_t)) / 3 + 2) + 11;
    // "LISTEN_PID=" - 11 characters; the expression above gives a conservative estimate
    // on the maxiumum number of bytes required for LISTEN=nnn, including nul terminator,
    // where nnn is a pid_t in decimal (i.e. one decimal digit is worth just over 3 bits).
    char nbuf[bufsz];
    
    // "DINIT_CS_FD=" - 12 bytes. (we -1 from sizeof(int) in account of sign bit).
    constexpr int csenvbufsz = ((CHAR_BIT * sizeof(int) - 1) / 3 + 2) + 12;
    char csenvbuf[csenvbufsz];
    
    int minfd = (socket_fd == -1) ? 3 : 4;

    // Move wpipefd/csfd to another fd if necessary
    if (wpipefd < minfd) {
        wpipefd = fcntl(wpipefd, F_DUPFD_CLOEXEC, minfd);
        if (wpipefd == -1) goto failure_out;
    }
    
    if (csfd != -1 && csfd < minfd) {
        csfd = fcntl(csfd, F_DUPFD, minfd);
        if (csfd == -1) goto failure_out;
    }
    
    if (socket_fd != -1) {
        
        if (dup2(socket_fd, 3) == -1) goto failure_out;
        if (socket_fd != 3) {
            close(socket_fd);
        }
        
        if (putenv(const_cast<char *>("LISTEN_FDS=1"))) goto failure_out;
        snprintf(nbuf, bufsz, "LISTEN_PID=%jd", static_cast<intmax_t>(getpid()));
        if (putenv(nbuf)) goto failure_out;
    }
    
    if (csfd != -1) {
        snprintf(csenvbuf, csenvbufsz, "DINIT_CS_FD=%d", csfd);
        if (putenv(csenvbuf)) goto failure_out;
    }

    if (! on_console) {
        // Re-set stdin, stdout, stderr
        close(0); close(1); close(2);

        if (open("/dev/null", O_RDONLY) == 0) {
            // stdin = 0. That's what we should have; proceed with opening
            // stdout and stderr.
            if (open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR) != 1) {
                goto failure_out;
            }
            if (dup2(1, 2) != 2) {
                goto failure_out;
            }
        }
        else goto failure_out;
        
        // We have the option of creating a session and process group, or just a new process
        // group. If we just create a new process group, the child process cannot make itself
        // a session leader if it wants to do that (eg getty/login will generally want this).
        // If we do neither, and we are running with a controlling terminal, a ^C or similar
        // will also affect the child process (which probably isn't so bad, though since we
        // will handle the shutdown ourselves it's not necessary). Creating a new session
        // (and a new process group as part of that) seems like a safe bet, and has the
        // advantage of letting us signal the process as part of a process group.
        setsid();
    }
    else {
        // "run on console" - run as a foreground job on the terminal/console device
        
        // if do_set_ctty is false, we are the session leader; we are probably running
        // as a user process. Don't create a new session leader in that case, and run
        // as part of the parent session. Otherwise, the new session cannot claim the
        // terminal as a controlling terminal (it is already claimed), meaning that it
        // will not see control signals from ^C etc.
        
        if (do_set_ctty) {
            // Disable suspend (^Z) (and on some systems, delayed suspend / ^Y)
            signal(SIGTSTP, SIG_IGN);
            
            // Become session leader
            setsid();
            ioctl(0, TIOCSCTTY, 0);
        }
        setpgid(0,0);
        tcsetpgrp(0, getpgrp());
    }
    
    sigprocmask(SIG_SETMASK, &sigwait_set, nullptr);
    
    execvp(args[0], const_cast<char **>(args));
    
    // If we got here, the exec failed:
    failure_out:
    int exec_status = errno;
    write(wpipefd, &exec_status, sizeof(int));
    _exit(0);
}

// Mark this and all dependent services as force-stopped.
void service_record::forceStop() noexcept
{
    if (service_state != service_state_t::STOPPED) {
        force_stop = true;
        services->addToStopQueue(this);
    }
}

void service_record::dependentStopped() noexcept
{
    if (service_state == service_state_t::STOPPING && waiting_for_deps) {
        services->addToStopQueue(this);
    }
}

void service_record::stop(bool bring_down) noexcept
{
    if (start_explicit) {
        start_explicit = false;
        release();
    }

    if (bring_down) {
        do_stop();
    }
}

void service_record::do_stop() noexcept
{
    if (pinned_started) return;

    if (start_explicit && ! do_auto_restart()) {
        start_explicit = false;
        release();
        if (required_by == 0) return; // release will re-call us anyway
    }

    if (service_state != service_state_t::STARTED) {
        if (service_state == service_state_t::STARTING) {
            if (! can_interrupt_start()) {
                // Well this is awkward: we're going to have to continue
                // starting, but we don't want any dependents to think that
                // they are still waiting to start.
                // Make sure they remain stopped:
                stopDependents();
                return;
            }

            // We must have had desired_state == STARTED.
            notifyListeners(service_event::STARTCANCELLED);
            
            interrupt_start();

            // Reaching this point, we are starting interruptibly - so we
            // stop now (by falling through to below).
        }
        else {
            // If we're starting we need to wait for that to complete.
            // If we're already stopping/stopped there's nothing to do.
            return;
        }
    }

    service_state = service_state_t::STOPPING;
    waiting_for_deps = true;
    if (stopDependents()) {
        services->addToStopQueue(this);
    }
}

bool service_record::stopCheckDependents() noexcept
{
    bool all_deps_stopped = true;
    for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
        if (! (*i)->is_stopped()) {
            all_deps_stopped = false;
            break;
        }
    }
    
    return all_deps_stopped;
}

bool service_record::stopDependents() noexcept
{
    bool all_deps_stopped = true;
    for (sr_iter i = dependents.begin(); i != dependents.end(); ++i) {
        if (! (*i)->is_stopped()) {
            // Note we check *first* since if the dependent service is not stopped,
            // 1. We will issue a stop to it shortly and
            // 2. It will notify us when stopped, at which point the stopCheckDependents()
            //    check is run anyway.
            all_deps_stopped = false;
        }

        if (force_stop) {
            // If this service is to be forcefully stopped, dependents must also be.
            (*i)->forceStop();
        }

        (*i)->prop_stop = true;
        services->addToPropQueue(*i);
    }

    return all_deps_stopped;
}

// All dependents have stopped; we can stop now, too. Only called when STOPPING.
void service_record::all_deps_stopped() noexcept
{
    waiting_for_deps = false;
    stopped();
}

void base_process_service::all_deps_stopped() noexcept
{
    waiting_for_deps = false;
    if (pid != -1) {
        // The process is still kicking on - must actually kill it. We signal the process
        // group (-pid) rather than just the process as there's less risk then of creating
        // an orphaned process group:
        if (! onstart_flags.no_sigterm) {
            kill(-pid, SIGTERM);
        }
        if (term_signal != -1) {
            kill(-pid, term_signal);
        }

        // In most cases, the rest is done in handle_exit_status.
        // If we are a BGPROCESS and the process is not our immediate child, however, that
        // won't work - check for this now:
        if (record_type == service_type::BGPROCESS) {
            // TODO use 'tracking_child' instead
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == -1 && errno == ECHILD) {
                // We can't track this child (or it's terminated already)
                stopped();
            }
            else if (r == pid) {
                // Process may have died due to signal since we explicitly requested it to
                // stop by signalling it; no need to log any termination status.
                stopped();
            }
        }
    }
    else {
        // The process is already dead.
        stopped();
    }
}

void scripted_service::all_deps_stopped() noexcept
{
    waiting_for_deps = false;
    if (stop_command.length() == 0) {
        stopped();
    }
    else if (! start_ps_process(stop_arg_parts, false)) {
        // Couldn't execute stop script, but there's not much we can do:
        stopped();
    }
}

void service_record::unpin() noexcept
{
    if (pinned_started) {
        pinned_started = false;
        if (desired_state == service_state_t::STOPPED) {
            do_stop();
            services->process_queues();
        }
    }
    if (pinned_stopped) {
        pinned_stopped = false;
        if (desired_state == service_state_t::STARTED) {
            do_start();
            services->process_queues();
        }
    }
}

void service_record::queue_for_console() noexcept
{
    services->append_console_queue(this);
}

void service_record::release_console() noexcept
{
    services->pull_console_queue();
}

void service_record::interrupt_start() noexcept
{
    services->unqueue_console(this);
}

void service_set::service_active(service_record *sr) noexcept
{
    active_services++;
}

void service_set::service_inactive(service_record *sr) noexcept
{
    active_services--;
}

base_process_service::base_process_service(service_set *sset, string name, service_type service_type_p, string &&command,
        std::list<std::pair<unsigned,unsigned>> &command_offsets,
        sr_list &&pdepends_on, const sr_list &pdepends_soft)
     : service_record(sset, name, service_type_p, std::move(command), command_offsets,
         std::move(pdepends_on), pdepends_soft), child_listener(this), child_status_listener(this)
{
    restart_interval_count = 0;
    restart_interval_time = {0, 0};
    restart_timer.service = this;
    restart_timer.add_timer(eventLoop);

    // By default, allow a maximum of 3 restarts within 10.0 seconds:
    restart_interval.tv_sec = 10;
    restart_interval.tv_nsec = 0;
    max_restart_interval_count = 3;
}

void base_process_service::do_restart() noexcept
{
    restarting = false;
    waiting_restart_timer = false;
    restart_interval_count++;

    // We may be STARTING (regular restart) or STARTED ("smooth recovery"). This affects whether
    // the process should be granted access to the console:
    bool on_console = service_state == service_state_t::STARTING
            ? onstart_flags.starts_on_console : onstart_flags.runs_on_console;

    if (! start_ps_process(exec_arg_parts, on_console)) {
        if (service_state == service_state_t::STARTING) {
            failed_to_start();
        }
        else {
            desired_state = service_state_t::STOPPED;
            forceStop();
        }
        services->process_queues();
    }
}

bool base_process_service::restart_ps_process() noexcept
{
    using time_val = eventloop_t::time_val;

    time_val current_time;
    eventLoop.get_time(current_time, clock_type::MONOTONIC);

    if (max_restart_interval_count != 0) {
        // Check whether we're still in the most recent restart check interval:
        time_val int_diff = current_time - restart_interval_time;
        if (int_diff < restart_interval) {
            if (restart_interval_count >= max_restart_interval_count) {
                log(LogLevel::ERROR, "Service ", service_name, " restarting too quickly; stopping.");
                return false;
            }
        }
        else {
            restart_interval_time = current_time;
            restart_interval_count = 0;
        }
    }

    // Check if enough time has lapsed since the prevous restart. If not, start a timer:
    time_val tdiff = current_time - last_start_time;
    if (restart_delay < tdiff) {
        // > restart delay (normally 200ms)
        do_restart();
    }
    else {
        time_val timeout = restart_delay - tdiff;
        restart_timer.arm_timer_rel(eventLoop, timeout);
        waiting_restart_timer = true;
    }
    return true;
}

void base_process_service::interrupt_start() noexcept
{
    // overridden in subclasses
    if (waiting_restart_timer) {
        restart_timer.stop_timer(eventLoop);
        waiting_restart_timer = false;
    }
    service_record::interrupt_start();
}

dasynq::rearm process_restart_timer::timer_expiry(eventloop_t &, int expiry_count)
{
    service->do_restart();
    return dasynq::rearm::DISARM;
}
