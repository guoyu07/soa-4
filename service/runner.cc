/* runner.cc                                                       -*- C++ -*-
   Wolfgang Sourdeau, September 2013
   Copyright (c) 2013 Datacratic.  All rights reserved.

   A command runner class that hides the specifics of the underlying unix
   system calls and can intercept input and output.
*/

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <iostream>
#include <utility>

#include "jml/arch/futex.h"
#include "jml/arch/timers.h"
#include "jml/utils/guard.h"
#include "jml/utils/file_functions.h"

#include "logs.h"
#include "message_loop.h"
#include "sink.h"

#include "runner.h"

#include "soa/types/basic_value_descriptions.h"

#include <future>

using namespace std;
using namespace Datacratic;


timevalDescription::
timevalDescription()
{
    addField("tv_sec", &timeval::tv_sec, "seconds");
    addField("tv_usec", &timeval::tv_usec, "micro seconds", (long)0);
}

rusageDescription::
rusageDescription()
{
    addField("utime", &rusage::ru_utime, "user CPU time used");
    addField("stime", &rusage::ru_stime, "system CPU time used");
    addField("maxrss", &rusage::ru_maxrss, "maximum resident set size", (long)0);
    addField("ixrss", &rusage::ru_ixrss, "integral shared memory size", (long)0);
    addField("idrss", &rusage::ru_idrss, "integral unshared data size", (long)0);
    addField("isrss", &rusage::ru_isrss, "integral unshared stack size", (long)0);
    addField("minflt", &rusage::ru_minflt, "page reclaims (soft page faults)", (long)0);
    addField("majflt", &rusage::ru_majflt, "page faults (hard page faults)", (long)0);
    addField("nswap", &rusage::ru_nswap, "swaps", (long)0);
    addField("inblock", &rusage::ru_inblock, "block input operations", (long)0);
    addField("oublock", &rusage::ru_oublock, "block output operations", (long)0);
    addField("msgsnd", &rusage::ru_msgsnd, "IPC messages sent", (long)0);
    addField("msgrcv", &rusage::ru_msgrcv, "IPC messages received", (long)0);
    addField("nsignals", &rusage::ru_nsignals, "signals received", (long)0);
    addField("nvcsw", &rusage::ru_nvcsw, "voluntary context switches", (long)0);
    addField("nivcsw", &rusage::ru_nivcsw, "involuntary context switches", (long)0);
}


namespace {

Logging::Category warnings("Runner::warning");

tuple<int, int>
CreateStdPipe(bool forWriting)
{
    int fds[2];
    int rc = pipe(fds);
    if (rc == -1) {
        throw ML::Exception(errno, "CreateStdPipe pipe2");
    }

    if (forWriting) {
        return tuple<int, int>(fds[1], fds[0]);
    }
    else {
        return tuple<int, int>(fds[0], fds[1]);
    }
}

} // namespace

namespace Datacratic {

/* ASYNCRUNNER */

Runner::
Runner()
    : EpollLoop(nullptr),
      closeStdin(false), running_(false),
      startDate_(Date::negativeInfinity()), endDate_(startDate_),
      childPid_(-1),
      statusRemaining_(sizeof(ProcessStatus))
{
}

Runner::
~Runner()
{
    kill(SIGTERM, false);
}

void
Runner::
handleChildStatus(const struct epoll_event & event)
{
    ProcessStatus status;

    if ((event.events & EPOLLIN) != 0) {
        while (1) {
            char * current = (statusBuffer_ + sizeof(ProcessStatus)
                              - statusRemaining_);
            ssize_t s = ::read(task_.statusFd, current, statusRemaining_);
            if (s == -1) {
                if (errno == EWOULDBLOCK) {
                    break;
                }
                else if (errno == EBADF || errno == EINVAL) {
                    /* This happens when the pipe or socket was closed by the
                       remote process before "read" was called (race
                       condition). */
                    break;
                }
                throw ML::Exception(errno, "Runner::handleChildStatus read");
            }
            else if (s == 0) {
                break;
            }

            statusRemaining_ -= s;

            if (statusRemaining_ > 0) {
                continue;
            }

            memcpy(&status, statusBuffer_, sizeof(status));

            // Set up for next message
            statusRemaining_ = sizeof(statusBuffer_);

            task_.statusState = status.state;
            task_.runResult.usage = status.usage;

            if (status.launchErrno
                || status.launchErrorCode != LaunchError::NONE) {
                // Error
                task_.runResult.updateFromLaunchError
                    (status.launchErrno,
                     strLaunchError(status.launchErrorCode));
                task_.statusState = ProcessState::STOPPED;
                childPid_ = -2;
                ML::futex_wake(childPid_);

                if (stdInSink_ && stdInSink_->state != OutputSink::CLOSED) {
                    stdInSink_->requestClose();
                }
                attemptTaskTermination();
                break;
            }

            switch (status.state) {
            case ProcessState::LAUNCHING:
                childPid_ = status.pid;
                break;
            case ProcessState::RUNNING:
                childPid_ = status.pid;
                ML::futex_wake(childPid_);
                break;
            case ProcessState::STOPPED:
                childPid_ = -3;
                ML::futex_wake(childPid_);
                task_.runResult.updateFromStatus(status.childStatus);
                task_.statusState = ProcessState::DONE;
                if (stdInSink_ && stdInSink_->state != OutputSink::CLOSED) {
                    stdInSink_->requestClose();
                }
                attemptTaskTermination();
                break;
            case ProcessState::DONE:
                throw ML::Exception("unexpected status DONE");
            case ProcessState::UNKNOWN:
                throw ML::Exception("unexpected status UNKNOWN");
            }

            if (status.launchErrno
                || status.launchErrorCode != LaunchError::NONE)
                break;
        }
    }

    if ((event.events & EPOLLHUP) != 0) {
        // This happens when the thread that launched the process exits,
        // and the child process follows.
        removeFd(task_.statusFd, true);
        ::close(task_.statusFd);
        task_.statusFd = -1;

        if (task_.statusState == ProcessState::RUNNING
            || task_.statusState == ProcessState::LAUNCHING) {

            cerr << "*************************************************************" << endl;
            cerr << " HANGUP ON STATUS FD: RUNNER FORK THREAD EXITED?" << endl;
            cerr << "*************************************************************" << endl;
            cerr << "state = " << jsonEncode(task_.runResult.state) << endl;
            cerr << "statusState = " << (int)task_.statusState << endl;
            cerr << "childPid_ = " << childPid_ << endl;

            // We will never get another event, so we need to clean up 
            // everything here.
            childPid_ = -3;
            ML::futex_wake(childPid_);

            task_.runResult.state = RunResult::PARENT_EXITED;
            task_.runResult.signum = SIGHUP;
            task_.statusState = ProcessState::DONE;
            if (stdInSink_ && stdInSink_->state != OutputSink::CLOSED) {
                stdInSink_->requestClose();
            }
            attemptTaskTermination();
        }
    }
}

void
Runner::
handleOutputStatus(const struct epoll_event & event,
                   int & outputFd, shared_ptr<InputSink> & sink)
{
    char buffer[4096];
    bool closedFd(false);
    string data;

    if ((event.events & EPOLLIN) != 0) {
        while (1) {
            ssize_t len = ::read(outputFd, buffer, sizeof(buffer));
            if (len < 0) {
                if (errno == EWOULDBLOCK) {
                    break;
                }
                else if (errno == EBADF || errno == EINVAL) {
                    /* This happens when the pipe or socket was closed by the
                       remote process before "read" was called (race
                       condition). */
                    closedFd = true;
                    break;
                }
                else {
                    throw ML::Exception(errno,
                                        "Runner::handleOutputStatus read");
                }
            }
            else if (len == 0) {
                closedFd = true;
                break;
            }
            else if (len > 0) {
                data.append(buffer, len);
            }
        }

        if (data.size() > 0) {
            sink->notifyReceived(move(data));
        }
    }

    if (closedFd || (event.events & EPOLLHUP) != 0) {
        ExcAssert(sink != nullptr);
        sink->notifyClosed();
        sink.reset();
        if (outputFd > -1) {
            removeFd(outputFd, true);
            ::close(outputFd);
            outputFd = -1;
        }
        attemptTaskTermination();
    }
}

void
Runner::
attemptTaskTermination()
{
    /* There is a set of things that occurs spontaneously when a process
       exits, due to the way Linux (possibly POSIX) handles processes, file
       descriptors, etc. For example, the stdout/stderr channels of a
       subprocess are always going to be closed when the relevant process
       exits and all the data written by the program will be flushed from the
       kernel buffers. Even though this may not occur in order, all of those
       events will occur and will be caught by our epoll queue. This is the
       basis of how we handle events throughout the Runner class.

       For a task to be considered done:
       - stdout and stderr must have been closed, provided we redirected them
       - the closing child status must have been returned
       - stdInSink must either be null or its state considered "closed"
       This is a requirement for absolutely *all* conditions: whether the
       calls to "fork" and "exec" have succeeded, whether the underlying
       program has been successfully run or not. But, although they are
       guaranteed to occur, those events do not specifically occur in a
       deterministic order.

       Since those checks must be performed at various places, the same
       conditions must all be checked all the time and the same operations
       must be performed when they are all met. This is what
       "attemptTaskTermination" does.
    */
    if ((!stdInSink_
         || stdInSink_->state == OutputSink::CLOSED
         || stdInSink_->state == OutputSink::CLOSING)
        && !stdOutSink_ && !stdErrSink_ && childPid_ < 0
        && (task_.statusState == ProcessState::STOPPED
            || task_.statusState == ProcessState::DONE)) {
        task_.postTerminate(*this);

        if (stdInSink_) {
            stdInSink_.reset();
        }

        running_ = false;
        endDate_ = Date::now();
        ML::futex_wake(running_);
    }
#if 0
    else {
        cerr << "cannot terminate yet because:\n";
        if ((stdInSink_ && stdInSink_->state != OutputSink::CLOSED)) {
            cerr << "stdin sink active\n";
        }
        if (stdOutSink_) {
            cerr << "stdout sink active\n";
        }
        if (stdErrSink_) {
            cerr << "stderr sink active\n";
        }
        if (childPid_ >= 0) {
            cerr << "childPid_ >= 0\n";
        }
        if (!(task_.statusState == ProcessState::STOPPED
              || task_.statusState == DONE)) {
            cerr << "task status != stopped/done\n";
        }
    }
#endif
}

OutputSink &
Runner::
getStdInSink()
{
    if (running_)
        throw ML::Exception("already running");
    if (stdInSink_)
        throw ML::Exception("stdin sink already set");

    auto onClose = [&] () {
        if (task_.stdInFd != -1) {
            ::close(task_.stdInFd);
            task_.stdInFd = -1;
        }
        removeFd(stdInSink_->selectFd(), true);
        attemptTaskTermination();
    };
    stdInSink_.reset(new AsyncFdOutputSink(onClose, onClose));

    return *stdInSink_;
}

void
Runner::
run(const vector<string> & command,
    const OnTerminate & onTerminate,
    const shared_ptr<InputSink> & stdOutSink,
    const shared_ptr<InputSink> & stdErrSink)
{
    if (parent_ == nullptr) {
        LOG(warnings)
            << ML::format("Runner %p is not connected to any MessageLoop\n", this);
    }

    runImpl(command, onTerminate, stdOutSink, stdErrSink);
    int counter = 1;
    while (childPid_ == -1) {
        ML::sleep(0.1);
        ++ counter;
        if (counter % 50 == 0) {
            cerr << "Runner waiting after child...\n";
            counter = 1;
        }
    }
}

RunResult
Runner::
runSync(const vector<string> & command,
        const shared_ptr<InputSink> & stdOutSink,
        const shared_ptr<InputSink> & stdErrSink,
        const string & stdInData)
{
    RunResult result;

    std::atomic<bool> terminated(false);
    auto onTerminate = [&] (const RunResult & newResult) {
        result = newResult;
        terminated = true;
    };

    OutputSink * sink(nullptr);
    if (stdInData.size() > 0) {
        sink = &getStdInSink();
    }
    runImpl(command, onTerminate, stdOutSink, stdErrSink);
    if (sink) {
        sink->write(stdInData);
        sink->requestClose();
    }

    while (!terminated) {
        loop(-1, -1);
    }

    return result;
}

void
Runner::
runImpl(const vector<string> & command,
        const OnTerminate & onTerminate,
        const shared_ptr<InputSink> & stdOutSink,
        const shared_ptr<InputSink> & stdErrSink)
{
    // Need shared ptr to avoid race between lambda exiting and the
    // rest of this function, causing done to be destroyed before
    // set_value() has returned.
    std::shared_ptr<std::promise<bool> > done(new std::promise<bool>());
    

    // We run this in the message loop thread, which becomes the parent
    // of the child process.  This is to avoid problems when the thread
    // we're calling run from exits, and since it's the parent process of
    // the fork, causes the subprocess to exit to due to
    // PR_SET_DEATHSIG being set
    auto toRun = [&] ()
        {
            try {
                this->doRunImpl(command, onTerminate, stdOutSink, stdErrSink);
                done->set_value(true);
            } JML_CATCH_ALL {
                done->set_exception(std::current_exception());
            }
        };

    if (parent_) {
        // Run it in the message loop thread
        bool res = parent_->runInMessageLoopThread(toRun);
        ExcAssert(res);
    }
    else toRun();
    
    // Wait for the function to finish
    std::future<bool> future = done->get_future();
    bool success = future.get();
    ExcAssert(success);
}

void
Runner::
doRunImpl(const vector<string> & command,
          const OnTerminate & onTerminate,
          const shared_ptr<InputSink> & stdOutSink,
          const shared_ptr<InputSink> & stdErrSink)
{
    if (running_)
        throw ML::Exception("already running");

    startDate_ = Date::now();
    endDate_ = Date::negativeInfinity();
    running_ = true;
    ML::futex_wake(running_);

    task_.statusState = ProcessState::UNKNOWN;
    task_.onTerminate = onTerminate;

    ProcessFds childFds;
    tie(task_.statusFd, childFds.statusFd) = CreateStdPipe(false);

    if (stdInSink_) {
        tie(task_.stdInFd, childFds.stdIn) = CreateStdPipe(true);
    }
    else if (closeStdin) {
        childFds.stdIn = -1;
    }
    if (stdOutSink) {
        stdOutSink_ = stdOutSink;
        tie(task_.stdOutFd, childFds.stdOut) = CreateStdPipe(false);
    }
    if (stdErrSink) {
        stdErrSink_ = stdErrSink;
        tie(task_.stdErrFd, childFds.stdErr) = CreateStdPipe(false);
    }

    ::flockfile(stdout);
    ::flockfile(stderr);
    ::fflush_unlocked(NULL);
    task_.wrapperPid = fork();
    ::funlockfile(stderr);
    ::funlockfile(stdout);
    if (task_.wrapperPid == -1) {
        throw ML::Exception(errno, "Runner::run fork");
    }
    else if (task_.wrapperPid == 0) {
        task_.runWrapper(command, childFds);
    }
    else {
        task_.statusState = ProcessState::LAUNCHING;

        ML::set_file_flag(task_.statusFd, O_NONBLOCK);
        if (stdInSink_) {
            ML::set_file_flag(task_.stdInFd, O_NONBLOCK);
            stdInSink_->init(task_.stdInFd);

            shared_ptr<AsyncFdOutputSink> stdinCopy = stdInSink_;
            auto stdinCb = [=] (const epoll_event & event) {
                stdinCopy->processOne();
            };
            addFd(stdInSink_->selectFd(), true, false, stdinCb);
        }
        auto statusCb = [&] (const epoll_event & event) {
            handleChildStatus(event);
        };
        addFd(task_.statusFd, true, false, statusCb);
        if (stdOutSink) {
            ML::set_file_flag(task_.stdOutFd, O_NONBLOCK);
            auto outputCb = [=] (const epoll_event & event) {
                handleOutputStatus(event, task_.stdOutFd, stdOutSink_);
            };
            addFd(task_.stdOutFd, true, false, outputCb);
        }
        if (stdErrSink) {
            ML::set_file_flag(task_.stdErrFd, O_NONBLOCK);
            auto outputCb = [=] (const epoll_event & event) {
                handleOutputStatus(event, task_.stdErrFd, stdErrSink_);
            };
            addFd(task_.stdErrFd, true, false, outputCb);
        }

        childFds.close();
    }
}

bool
Runner::
kill(int signum, bool mustSucceed) const
{
    if (childPid_ <= 0) {
        if (mustSucceed)
            throw ML::Exception("subprocess not available");
        else return false;
    }

    ::kill(-childPid_, signum);
    waitTermination();
    return true;
}

bool
Runner::
signal(int signum, bool mustSucceed)
{
    if (childPid_ <= 0) {
        if (mustSucceed)
            throw ML::Exception("subprocess not available");
        else return false;
    }
    
    ::kill(childPid_, signum);
    return true;
}

bool
Runner::
waitStart(double secondsToWait) const
{
    Date deadline = Date::now().plusSeconds(secondsToWait);

    while (childPid_ == -1) {
        double timeToWait = Date::now().secondsUntil(deadline);
        if (timeToWait < 0)
            break;
        if (isfinite(timeToWait))
            ML::futex_wait(childPid_, -1, timeToWait);
        else ML::futex_wait(childPid_, -1);
    }

    return childPid_ > 0;
}

void
Runner::
waitTermination() const
{
    while (running_) {
        ML::futex_wait(running_, true);
    }
}

double
Runner::
duration()
    const
{
    Date end = Date::now();
    if (!running_) {
        end = endDate_;
    }

    return (end - startDate_);
}

/* RUNNER::TASK */

Runner::Task::
Task()
    : wrapperPid(-1),
      stdInFd(-1),
      stdOutFd(-1),
      stdErrFd(-1),
      statusFd(-1),
      statusState(ProcessState::UNKNOWN)
{}

void
Runner::Task::
runWrapper(const vector<string> & command, ProcessFds & fds)
{
    auto dieWithErrno = [&] (const char * message) {
        ProcessStatus status;

        status.state = ProcessState::STOPPED;
        status.setErrorCodes(errno, LaunchError::SUBTASK_LAUNCH);
        fds.writeStatus(status);

        throw ML::Exception(errno, message);
    };

    // Find runner_helper path
    char exeBuffer[16384];
    ssize_t len;
    {
        char * res = getcwd(exeBuffer, 16384);
        ExcAssert(res != NULL);
        len = ::strlen(exeBuffer);
        static const char * appendStr = "/" BIN "/runner_helper";
        ::strcpy(&exeBuffer[len], appendStr);
    }

    {
        // Make sure the deduced path is right
        struct stat sb;
        int res = stat(exeBuffer, &sb);
        if (res != 0) {
            string msg = "Runner error: Failed to find runner_helper. errno:"
                         + to_string(errno) + " path:" + exeBuffer;
            cerr << msg << endl;
            throw ML::Exception(msg);
        }
    }
    vector<string> preArgs = { /*"gdb", "--tty", "/dev/pts/48", "--args"*/ /*"../strace-code/strace", "-b", "execve", "-ftttT", "-o", "runner_helper.strace"*/ };


    // Set up the arguments before we fork, as we don't want to call malloc()
    // from the fork, and it can be called from c_str() in theory.
    len = command.size();
    char * argv[len + 3 + preArgs.size()];

    for (unsigned i = 0;  i < preArgs.size();  ++i)
        argv[i] = (char *)preArgs[i].c_str();

    int idx = preArgs.size();

    argv[idx++] = exeBuffer;

    size_t channelsSize = 4*2*4+3+1;
    char channels[channelsSize];
    fds.encodeToBuffer(channels, channelsSize);
    argv[idx++] = channels;

    for (int i = 0; i < len; i++) {
        argv[idx++] = (char *) command[i].c_str();
    }
    argv[idx++] = nullptr;

    std::vector<char *> env;

    char * const * p = environ;

    while (*p) {
        env.push_back(*p);
        ++p;
    }

    env.push_back(nullptr);

    char * const * envp = &env[0];

    int res = execve(argv[0], argv, envp);
    if (res == -1) {
        dieWithErrno("launching runner helper");
    }

    throw ML::Exception("You are the King of Time!");
}

/* This method *must* be called from attemptTaskTermination, in order to
 * respect the natural order of things. */
void
Runner::Task::
postTerminate(Runner & runner)
{
    if (wrapperPid <= 0) {
        throw ML::Exception("wrapperPid <= 0, has postTerminate been executed before?");
    }

    int wrapperPidStatus;
    while (true) {
        int res = ::waitpid(wrapperPid, &wrapperPidStatus, 0);
        if (res == wrapperPid) {
            break;
        }
        else if (res == -1) {
            if (errno != EINTR) {
                throw ML::Exception(errno, "waitpid");
            }
        }
        else {
            throw ML::Exception("waitpid has not returned the wrappedPid");
        }
    }
    wrapperPid = -1;

    if (stdInFd != -1) {
        runner.removeFd(stdInFd, true);
        ::close(stdInFd);
        stdInFd = -1;
    }

    auto unregisterFd = [&] (int & fd) {
        if (fd > -1) {
            JML_TRACE_EXCEPTIONS(false);
            try {
                runner.removeFd(fd, true);
            }
            catch (const ML::Exception & exc) {
            }
            ::close(fd);
            fd = -1;
        }
    };
    unregisterFd(stdOutFd);
    unregisterFd(stdErrFd);

    command.clear();

    if (onTerminate) {
        onTerminate(runResult);
        onTerminate = nullptr;
    }
    runResult = RunResult();
}


/* RUNRESULT */

RunResult::
RunResult()
    : state(UNKNOWN), signum(-1), returnCode(-1), launchErrno(0)
{
    ::memset(&usage, 0, sizeof(usage));
}

void
RunResult::
updateFromStatus(int status)
{
    if (WIFEXITED(status)) {
        state = RETURNED;
        returnCode = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status)) {
        state = SIGNALED;
        signum = WTERMSIG(status);
    }
}

int
RunResult::
processStatus()
    const
{
    int status;

    if (state == RETURNED)
        status = returnCode;
    else if (state == SIGNALED)
        status = 128 + signum;
    else if (state == LAUNCH_ERROR) {
        if (launchErrno == EPERM) {
            status = 126;
        }
        else if (launchErrno == ENOENT) {
            status = 127;
        }
        else {
            status = 1;
        }
    }
    else
        throw ML::Exception("unhandled state");

    return status;
}

void
RunResult::
updateFromLaunchError(int launchErrno,
                      const std::string & launchError)
{
    this->state = LAUNCH_ERROR;
    this->launchErrno = launchErrno;
    if (!launchError.empty()) {
        this->launchError = launchError;
        if (launchErrno)
            this->launchError += std::string(": ")
                + strerror(launchErrno);
    }
    else {
        this->launchError = strerror(launchErrno);
    }
}

std::string
to_string(const RunResult::State & state)
{
    switch (state) {
    case RunResult::UNKNOWN: return "UNKNOWN";
    case RunResult::LAUNCH_ERROR: return "LAUNCH_ERROR";
    case RunResult::RETURNED: return "RETURNED";
    case RunResult::SIGNALED: return "SIGNALED";
    case RunResult::PARENT_EXITED: return "PARENT_EXITED";
    }

    return ML::format("RunResult::State(%d)", state);
}

std::ostream &
operator << (std::ostream & stream, const RunResult::State & state)
{
    return stream << to_string(state);
}

RunResultDescription::
RunResultDescription()
{
    addField("state", &RunResult::state, "State of run command");
    addField("signum", &RunResult::signum,
             "Signal number that it exited with", -1);
    addField("returnCode", &RunResult::returnCode,
             "Return code of command", -1);
    addField("launchErrno", &RunResult::launchErrno,
             "Errno for launch error", 0);
    addField("launchError", &RunResult::launchError,
             "Error message for launch error");
    addField("usage", &RunResult::usage,
             "Process statistics as returned by getrusage()");
}

RunResultStateDescription::
RunResultStateDescription()
{
    addValue("UNKNOWN", RunResult::UNKNOWN,
             "State is unknown or uninitialized");
    addValue("LAUNCH_ERROR", RunResult::LAUNCH_ERROR,
             "Command was unable to be launched");
    addValue("RETURNED", RunResult::RETURNED, "Command returned");
    addValue("SIGNALED", RunResult::SIGNALED, "Command exited with a signal");
    addValue("PARENT_EXITED", RunResult::PARENT_EXITED, "Parent process exited forcing child to die");
}


/* EXECUTE */

RunResult
execute(MessageLoop & loop,
        const vector<string> & command,
        const shared_ptr<InputSink> & stdOutSink,
        const shared_ptr<InputSink> & stdErrSink,
        const string & stdInData,
        bool closeStdin)
{
    static bool notified(false);

    if (!notified) {
        cerr << "warning: the \"MessageLoop\"-based \"execute\" function is deprecated\n";
        notified = true;
    }

    return execute(command, stdOutSink, stdErrSink, stdInData, closeStdin);
}

RunResult
execute(const vector<string> & command,
        const shared_ptr<InputSink> & stdOutSink,
        const shared_ptr<InputSink> & stdErrSink,
        const string & stdInData,
        bool closeStdin)
{
    Runner runner;

    runner.closeStdin = closeStdin;

    return runner.runSync(command, stdOutSink, stdErrSink, stdInData);
}

} // namespace Datacratic
