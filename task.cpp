#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "subsystem.hpp"
#include "util/log.hpp"
#include "util/mount.hpp"
#include "util/folder.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"
#include "util/crc32.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <grp.h>
#include <linux/kdev_t.h>
#include <net/if.h>
#include <linux/capability.h>
}

using std::stringstream;
using std::string;
using std::vector;
using std::map;

static int lastCap;

// TTaskEnv

TError TTaskEnv::GetGroupList() {
    int ngroups = 0;
    (void)getgrouplist(User.c_str(), Cred.Gid,
                       nullptr, &ngroups);

    GroupList = std::unique_ptr<TScopedMem>(new TScopedMem(ngroups * sizeof(gid_t)));
    if (getgrouplist(User.c_str(), Cred.Gid,
                     (gid_t*)GroupList->GetData(), &ngroups) < 0) {
        return TError(EError::Unknown, errno, "Can't get supplementary group list");
    }

    return TError::Success();
}

TError TTaskEnv::Prepare(const TCred &cred) {
    if (Command.empty())
        return TError::Success();

    Cred = cred;

    TError error = GetGroupList();
    if (error)
        return error;

    return TError::Success();
}

const char** TTaskEnv::GetEnvp() const {
    auto envp = new const char* [Environ.size() + 1];
    for (size_t i = 0; i < Environ.size(); i++)
        envp[i] = strdup(Environ[i].c_str());
    envp[Environ.size()] = NULL;

    return envp;
}

bool TTaskEnv::EnvHasKey(const std::string &key) {
    for (auto str : Environ) {
        std::string envKey;

        std::vector<std::string> tok;
        TError error = SplitString(str, '=', tok, 2);
        if (error)
            envKey = str;
        else
            envKey = tok[0];

        if (key == envKey)
            return true;
    }

    return false;
}

// TTask

void TTask::ReportPid(int pid) const {
    if (write(Wfd, &pid, sizeof(pid)) != sizeof(pid)) {
        L_ERR() << "partial write of pid: " << std::to_string(pid) << std::endl;
    }
}

void TTask::Abort(const TError &error) const {
    TError ret = error.Serialize(Wfd);
    if (ret)
        L_ERR() << ret << std::endl;
    exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    SetProcessName("portod-spawn-c");

    TTask *task = static_cast<TTask*>(arg);
    TError error = task->ChildCallback();
    task->Abort(error);
    return EXIT_FAILURE;
}

TError TTask::CreateTmpDir(const TPath &path, std::shared_ptr<TFolder> &dir) const {
    bool cleanup = path.ToString().find(config().container().tmp_dir()) == 0;

    dir = std::make_shared<TFolder>(path, cleanup);
    if (!dir->Exists()) {
        TError error = dir->Create(0755, true);
        if (error)
            return error;
        error = path.Chown(Env->Cred.Uid, Env->Cred.Gid);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildOpenStdFile(const TPath &path, int expected) {
    int ret = open(path.ToString().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0660);
    if (ret < 0)
        return TError(EError::InvalidValue, errno,
                      "open(" + path.ToString() + ") -> " +
                      std::to_string(expected));

    if (ret != expected)
        return TError(EError::Unknown, EINVAL,
                      "open(" + path.ToString() + ") -> " +
                      std::to_string(expected) + ": unexpected fd " +
                      std::to_string(ret));

    ret = fchown(ret, Env->Cred.Uid, Env->Cred.Gid);
    if (ret < 0)
        return TError(EError::Unknown, errno,
                      "fchown(" + path.ToString() + ") -> " +
                      std::to_string(expected));

    return TError::Success();
}

TError TTask::ReopenStdio() {
    CloseFds(3, { Wfd, TLogger::GetFd() });

    int ret = open(Env->StdinPath.ToString().c_str(), O_CREAT | O_RDONLY, 0660);
    if (ret < 0)
        return TError(EError::Unknown, errno, "open(" + Env->StdinPath.ToString() + ") -> 0");

    if (ret != 0)
        return TError(EError::Unknown, EINVAL, "open(0): unexpected fd");

    TError error = ChildOpenStdFile(Env->StdoutPath, 1);
    if (error)
        return error;
    error = ChildOpenStdFile(Env->StderrPath, 2);
    if (error)
        return error;

    return TError::Success();
}

TError TTask::ChildApplyCapabilities() {
    uint64_t effective, permitted, inheritable;

    if (!Env->Cred.IsRoot())
        return TError::Success();

    PORTO_ASSERT(lastCap != 0);

    effective = permitted = -1;
    inheritable = Env->Caps;

    TError error = SetCap(effective, permitted, inheritable);
    if (error)
        return error;

    for (int i = 0; i <= lastCap; i++) {
        if (!(Env->Caps & (1ULL << i)) && i != CAP_SETPCAP) {
            TError error = DropBoundedCap(i);
            if (error)
                return error;
        }
    }

    if (!(Env->Caps & (1ULL << CAP_SETPCAP))) {
        TError error = DropBoundedCap(CAP_SETPCAP);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildDropPriveleges() {
    if (setgid(Env->Cred.Gid) < 0)
        return TError(EError::Unknown, errno, "setgid()");

    auto groupnr = Env->GroupList->GetSize() / sizeof(gid_t);
    if (config().log().verbose()) {
        auto gid = (const gid_t *)Env->GroupList->GetData();
        for (unsigned i = 0; i < groupnr; i++)
            L() << "supplemetary_group[" << i << "]=" << gid[i] << std::endl;
    }

    if (setgroups(groupnr, (const gid_t*)Env->GroupList->GetData()) < 0)
        return TError(EError::Unknown, errno, "setgroups()");

    if (setuid(Env->Cred.Uid) < 0)
        return TError(EError::Unknown, errno, "setuid()");

    return TError::Success();
}

TError TTask::ChildExec() {
    clearenv();

    for (auto &s : Env->Environ) {
        char *d = strdup(s.c_str());
        putenv(d);
    }

	wordexp_t result;

	int ret = wordexp(Env->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        return TError(EError::Unknown, EINVAL, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }");
    case WRDE_BADVAL:
        return TError(EError::Unknown, EINVAL, "wordexp(): undefined shell variable was referenced");
    case WRDE_CMDSUB:
        return TError(EError::Unknown, EINVAL, "wordexp(): command substitution is not supported");
    case WRDE_SYNTAX:
        return TError(EError::Unknown, EINVAL, "wordexp(): syntax error");
    default:
    case WRDE_NOSPACE:
        return TError(EError::Unknown, EINVAL, "wordexp(): error " + std::to_string(ret));
    case 0:
        break;
    }

    auto envp = Env->GetEnvp();
    if (config().log().verbose()) {
        L() << "command=" << Env->Command << std::endl;
        for (unsigned i = 0; result.we_wordv[i]; i++)
            L() << "argv[" << i << "]=" << result.we_wordv[i] << std::endl;
        for (unsigned i = 0; envp[i]; i++)
            L() << "environ[" << i << "]=" << envp[i] << std::endl;
    }
    SetDieOnParentExit(0);
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, (char *const *)envp);

    return TError(EError::InvalidValue, errno, string("execvpe(") + result.we_wordv[0] + ", " + std::to_string(result.we_wordc) + ", " + std::to_string(Env->Environ.size()) + ")");
}

TError TTask::ChildBindDns() {
    vector<string> files = { "/etc/hosts", "/etc/resolv.conf" };

    for (auto &file : files) {
        TMount mnt(file, Env->Root + file, "none", {});
        TError error = mnt.BindFile(true);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildBindDirectores() {
    for (auto &bindMap : Env->BindMap) {
        TPath dest;

        if (bindMap.Dest.IsAbsolute())
            dest = Env->Root / bindMap.Dest;
        else
            dest = Env->Root / Env->Cwd / bindMap.Dest;

        if (!StringStartsWith(dest.RealPath().ToString(), Env->Root.ToString()))
            return TError(EError::InvalidValue, "Container bind mount "
                          + bindMap.Source.ToString() + " resolves to root "
                          + dest.RealPath().ToString()
                          + " (" + Env->Root.ToString() + ")");

        TMount mnt(bindMap.Source, dest, "none", {});

        TError error;
        if (bindMap.Source.GetType() == EFileType::Directory)
            error = mnt.BindDir(bindMap.Rdonly);
        else
            error = mnt.BindFile(bindMap.Rdonly);
        if (error)
            return error;

        // drop nosuid,noexec,nodev from volumes
        if (Env->NewMountNs) {
            error = TMount::Remount(dest, MS_REMOUNT | MS_BIND |
                                    (bindMap.Rdonly ? MS_RDONLY : 0));
            if (error)
                return error;
        }
    }

    return TError::Success();
}

TError TTask::ChildCreateNode(const TPath &path, unsigned int mode, unsigned int dev) {
    if (mknod(path.ToString().c_str(), mode, dev) < 0)
        return TError(EError::Unknown, errno, "mknod(" + path.ToString() + ")");

    return TError::Success();
}

static const std::vector<std::string> roproc = { "/proc/sysrq-trigger", "/proc/irq", "/proc/bus" };

TError TTask::ChildRestrictProc(bool restrictProcSys) {
    std::vector<std::string> dirs = roproc;

    if (restrictProcSys)
        dirs.push_back("/proc/sys");

    for (auto &path : dirs) {
        TMount mnt(Env->Root + path, Env->Root + path, "none", {});
        TError error = mnt.BindFile(true);
        if (error)
            return error;
    }

    TMount mnt("/dev/null", Env->Root + "/proc/kcore", "", {});
    TError error = mnt.Bind(false);
    if (error)
        return error;

    return TError::Success();
}

TError TTask::ChildMountRun() {
    TPath run = Env->Root + "/run";
    std::vector<std::string> subdirs;
    TFolder dir(run);
    if (!dir.Exists()) {
        TError error = dir.Create();
        if (error)
            return error;
    } else {
        TError error = dir.Items(EFileType::Directory, subdirs);
        if (error)
            return error;
    }

    TMount dev("tmpfs", run, "tmpfs", { "mode=755", "size=32m" });
    TError error = dev.MountDir(MS_NOSUID | MS_STRICTATIME);
    if (error)
        return error;

    for (auto name : subdirs) {
        TFolder d(run + "/" + name);
        TError error = d.Create();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildMountDev() {
    struct {
        const std::string path;
        unsigned int mode;
        unsigned int dev;
    } node[] = {
        { "/dev/null",    0666 | S_IFCHR, MKDEV(1, 3) },
        { "/dev/zero",    0666 | S_IFCHR, MKDEV(1, 5) },
        { "/dev/full",    0666 | S_IFCHR, MKDEV(1, 7) },
        { "/dev/random",  0666 | S_IFCHR, MKDEV(1, 8) },
        { "/dev/urandom", 0666 | S_IFCHR, MKDEV(1, 9) },
    };

    TMount dev("tmpfs", Env->Root + "/dev", "tmpfs", { "mode=755", "size=32m" });
    TError error = dev.MountDir(MS_NOSUID | MS_STRICTATIME);
    if (error)
        return error;

    TMount devpts("devpts", Env->Root + "/dev/pts", "devpts",
                  { "newinstance", "ptmxmode=0666", "mode=620" ,"gid=5" });
    error = devpts.MountDir(MS_NOSUID | MS_NOEXEC);
    if (error)
        return error;

    for (size_t i = 0; i < sizeof(node) / sizeof(node[0]); i++) {
        error = ChildCreateNode(Env->Root + node[i].path,
                           node[i].mode, node[i].dev);
        if (error)
            return error;
    }

    TPath ptmx = Env->Root + "/dev/ptmx";
    if (symlink("pts/ptmx", ptmx.ToString().c_str()) < 0)
        return TError(EError::Unknown, errno, "symlink(/dev/pts/ptmx)");

    TPath fd = Env->Root + "/dev/fd";
    if (symlink("/proc/self/fd", fd.ToString().c_str()) < 0)
        return TError(EError::Unknown, errno, "symlink(/dev/fd)");

    TFile f(Env->Root + "/dev/console", 0755);
    (void)f.Touch();

    return TError::Success();
}

TError TTask::ChildRemountRootRo() {
    if (!Env->RootRdOnly || !Env->Loop.IsEmpty())
        return TError::Success();

    // remount everything except binds to ro
    std::vector<std::shared_ptr<TMount>> snapshot;
    TError error = TMount::Snapshot(snapshot);
    if (error)
        return error;

    for (auto mnt : snapshot) {
        TPath path = Env->Root.InnerPath(mnt->GetMountpoint());
        if (path.IsEmpty())
            continue;

        bool skip = false;
        for (auto dir : roproc) {
            if (!path.InnerPath(dir).IsEmpty()) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;

        for (auto &bindMap : Env->BindMap) {
            TPath dest = bindMap.Dest;

            if (dest.NormalPath() == path.NormalPath()) {
                skip = true;
                break;
            }
        }

        if (skip)
            continue;

        L_ACT() << "Remount " << path << " ro" << std::endl;
        error =  TMount::Remount(mnt->GetMountpoint(),
                                 MS_REMOUNT | MS_BIND | MS_RDONLY);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildMountRootFs() {

    if (Env->Root.IsRoot())
        return TError::Success();

    if (!Env->Loop.IsEmpty()) {
        TLoopMount m(Env->Loop, Env->Root, "ext4", Env->LoopDev);
        TError error = m.Mount(Env->RootRdOnly);
        if (error)
            return error;
    } else {
        TMount root(Env->Root, Env->Root, "none", {});
        TError error = root.BindDir(false);
        if (error)
            return error;
    }

    unsigned long defaultFlags = MS_NOEXEC | MS_NOSUID | MS_NODEV;
    unsigned long sysfsFlags = defaultFlags | MS_RDONLY;

    TMount sysfs("sysfs", Env->Root + "/sys", "sysfs", {});
    TError error = sysfs.MountDir(sysfsFlags);
    if (error)
        return error;

    TMount proc("proc", Env->Root + "/proc", "proc", {});
    error = proc.MountDir(defaultFlags);
    if (error)
        return error;

    bool privileged = Env->Cred.IsRoot();
    error = ChildRestrictProc(!privileged);
    if (error)
        return error;

    error = ChildMountDev();
    if (error)
        return error;

    if (!Env->Loop.IsEmpty()) {
        error = ChildMountRun();
        if (error)
            return error;
    }

    TMount shm("shm", Env->Root + "/dev/shm", "tmpfs",
               { "mode=1777", "size=65536k" });
    error = shm.MountDir(defaultFlags);
    if (error)
        return error;

    if (Env->BindDns) {
        error = ChildBindDns();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildIsolateFs() {

    if (Env->Root.IsRoot())
        return TError::Success();

    TError error = PivotRoot(Env->Root);
    if (error) {
        L_WRN() << "Can't pivot root, roll back to chroot: " << error << std::endl;

        error = Env->Root.Chroot();
        if (error)
            return error;
    }

    // Allow suid binaries and device nodes at container root.
    error = TMount::Remount("/", MS_REMOUNT | MS_BIND |
                                 (Env->RootRdOnly ? MS_RDONLY : 0));
    if (error) {
        L_ERR() << "Can't remount / as suid and dev:" << error << std::endl;
        return error;
    }

    TPath newRoot("/");
    return newRoot.Chdir();
}

TError TTask::ChildEnableNet() {
    auto nl = std::make_shared<TNl>();
    if (!nl)
        throw std::bad_alloc();

    TError error = nl->Connect();
    if (error)
        return error;

    std::vector<std::string> devices = nl->FindLink(0);
    for (auto &dev : devices) {
        auto link = std::make_shared<TNlLink>(nl, dev);

        TError error = link->Load();
        if (error)
            return error;

        error = link->Up();
        if (error)
            return error;

        for (auto ip : Env->IpVec) {
            if (ip.Addr.IsEmpty())
                continue;

            if (ip.Iface == dev) {
                TError error = link->SetIpAddr(ip.Addr, ip.Prefix);
                if (error)
                    return error;
            }
        }

        for (auto gw : Env->GwVec) {
            if (gw.Addr.IsEmpty())
                continue;

            if (gw.Iface == dev) {
                error = link->SetDefaultGw(gw.Addr);
                if (error)
                    return error;
            }
        }
    }

    return TError::Success();
}

static std::string GenerateHw(const std::string &host, const std::string &name) {
    uint32_t n = Crc32(name);
    uint32_t h = Crc32(host);

    char buf[32];

    sprintf(buf, "02:%02x:%02x:%02x:%02x:%02x",
            (n & 0x000000FF) >> 0,
            (h & 0xFF000000) >> 24,
            (h & 0x00FF0000) >> 16,
            (h & 0x0000FF00) >> 8,
            (h & 0x000000FF) >> 0);

    return std::string(buf);
}

TError TTask::IsolateNet(int childPid) {
    auto nl = std::make_shared<TNl>();
    if (!nl)
        throw std::bad_alloc();

    TError error = nl->Connect();
    if (error)
        return error;


    for (auto &host : Env->NetCfg.HostIface) {
        auto link = std::make_shared<TNlLink>(nl, host.Dev);
        TError error = link->ChangeNs(host.Dev, childPid);
        if (error)
            return error;
    }

    for (auto &ipvlan : Env->NetCfg.IpVlan) {
        auto link = std::make_shared<TNlLink>(nl, "piv" + std::to_string(GetTid()));
        (void)link->Remove();

        TError error = link->AddIpVlan(ipvlan.Master, ipvlan.Mode, ipvlan.Mtu);
        if (error)
            return error;

        error = link->ChangeNs(ipvlan.Name, childPid);
        if (error) {
            (void)link->Remove();
            return error;
        }
    }

    std::string hostname = GetHostName();

    for (auto &mvlan : Env->NetCfg.MacVlan) {
        auto link = std::make_shared<TNlLink>(nl, "pmv" + std::to_string(GetTid()));
        (void)link->Remove();

        string hw = mvlan.Hw;
        if (hw.empty())
            hw = GenerateHw(Env->Hostname, mvlan.Master + mvlan.Name);

        L() << "Using " << (hw.empty() ? " generated " : "") << hw << " for " << mvlan.Name << "@" << mvlan.Master << std::endl;

        TError error = link->AddMacVlan(mvlan.Master, mvlan.Type, hw, mvlan.Mtu);
        if (error)
            return error;

        error = link->ChangeNs(mvlan.Name, childPid);
        if (error) {
            (void)link->Remove();
            return error;
        }
    }

    for (auto &veth : Env->NetCfg.Veth) {
        auto bridge = std::make_shared<TNlLink>(nl, veth.Bridge);
        TError error = bridge->Load();
        if (error)
            return error;

        string hw = veth.Hw;
        if (hw.empty())
            hw = GenerateHw(Env->Hostname, veth.Name + veth.Peer);

        if (config().network().debug())
            L() << "Using " << hw << " for " << veth.Name << " -> " << veth.Peer << std::endl;

        error = bridge->AddVeth(veth.Name, veth.Peer, hw, veth.Mtu, childPid);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildApplyLimits() {
    for (auto pair : Env->Rlimit) {
        int ret = setrlimit(pair.first, &pair.second);
        if (ret < 0)
            return TError(EError::Unknown, errno,
                          "setrlimit(" + std::to_string(pair.first) +
                          ", " + std::to_string(pair.second.rlim_cur) +
                          ":" + std::to_string(pair.second.rlim_max) + ")");
    }

    return TError::Success();
}

TError TTask::ChildSetHostname() {
    if (Env->Hostname == "" || Env->Root.ToString() == "/")
        return TError::Success();

    TFile f("/etc/hostname");
    if (f.Exists()) {
        string host = Env->Hostname + "\n";
        TError error = f.WriteStringNoAppend(host);
        if (error)
            return TError(EError::Unknown, error, "write(/etc/hostname)");
    }

    if (sethostname(Env->Hostname.c_str(), Env->Hostname.length()) < 0)
        return TError(EError::Unknown, errno, "sethostname()");

    return TError::Success();
}

TError TTask::ChildPrepareLoop() {
    if (!Env->Loop.IsEmpty()) {
        TFolder f(Env->Root);
        if (!f.Exists()) {
            TError error = f.Create(0755, true);
            if (error)
                return error;
        }
    }

    return TError::Success();
}

TError TTask::ChildCallback() {
    int ret;
    close(WaitParentWfd);
    if (read(WaitParentRfd, &ret, sizeof(ret)) != sizeof(ret))
        return TError(EError::Unknown, errno ?: ENODATA, "partial read from child sync pipe");

    close(Rfd);
    ResetAllSignalHandlers();
    TError error = ChildApplyLimits();
    if (error)
        return error;

    if (setsid() < 0)
        return TError(EError::Unknown, errno, "setsid()");

    umask(0);

    if (Env->NewMountNs) {
        // Remount to slave to receive propogations from parent namespace
        error = TMount::Remount("/", MS_REC | MS_SLAVE);
        if (error)
            return error;
    }

    if (Env->Isolate) {
        // remount proc so PID namespace works
        TMount tmpProc("proc", "/proc", "proc", {});
        if (tmpProc.Detach())
            return TError(EError::Unknown, errno, "datach procfs");
        if (tmpProc.MountDir())
            return TError(EError::Unknown, errno, "remount procfs");
    }

    if (Env->Isolate) {
        error = ChildPrepareLoop();
        if (error)
            return error;
    }

    if (Env->NetCfg.NewNetNs) {
        error = ChildEnableNet();
        if (error)
            return error;
    }

    if (Env->ParentNs.Mnt.IsOpened()) {
        error = Env->ParentNs.Mnt.SetNs();
        if (error)
            return error;

        error = Env->ParentNs.Root.Chroot();
        if (error)
            return error;

        error = Env->Cwd.Chdir();
        if (error)
            return error;
    } else {
        error = ChildMountRootFs();
        if (error)
            return error;

        error = ChildBindDirectores();
        if (error)
            return error;

        error = ChildRemountRootRo();
        if (error)
            return error;

        error = ChildIsolateFs();
        if (error)
            return error;

        error = Env->Cwd.Chdir();
        if (error)
            return error;

        error = ChildSetHostname();
        if (error)
            return error;
    }

    if (Env->NewMountNs) {
        // Make all shared: subcontainers will get propgation from us
        error = TMount::Remount("/", MS_REC | MS_SHARED);
        if (error)
            return error;
    }

    error = ChildApplyCapabilities();
    if (error)
        return error;

    error = ChildDropPriveleges();
    if (error)
        return error;

    return ChildExec();
}

TError TTask::CreateCwd() {
    return CreateTmpDir(Env->Cwd, Cwd);
}

TError TTask::Start() {
    int ret;
    int pfd[2], syncfd[2];

    Pid = 0;

    if (Env->CreateCwd) {
        TError error = CreateCwd();
        if (error) {
            if (error.GetError() != EError::NoSpace)
                L_ERR() << "Can't create temporary cwd: " << error << std::endl;
            return error;
        }
    }

    ExitStatus = 0;

    ret = pipe2(pfd, O_CLOEXEC);
    if (ret) {
        TError error(EError::Unknown, errno, "pipe2(pdf)");
        L_ERR() << "Can't create communication pipe for child: " << error << std::endl;
        return error;
    }

    Rfd = pfd[0];
    Wfd = pfd[1];

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    pid_t forkPid = fork();
    if (forkPid < 0) {
        TError error(EError::Unknown, errno, "fork()");
        L() << "Can't spawn child: " << error << std::endl;
        close(Rfd);
        close(Wfd);
        return error;
    } else if (forkPid == 0) {
        TError error;

        SetDieOnParentExit(SIGKILL);
        SetProcessName("portod-spawn-p");

        char stack[8192];

        (void)setsid();

        // move to target cgroups
        for (auto cg : Env->LeafCgroups) {
            error = cg.second->Attach(getpid());
            if (error) {
                L() << "Can't attach to cgroup: " << error << std::endl;
                ReportPid(-1);
                Abort(error);
            }
        }

        error = Env->ClientMntNs.SetNs();
        if (error) {
            L() << "Can't move task to client mount namespace: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        error = ReopenStdio();
        if (error) {
            ReportPid(-1);
            Abort(error);
        }

        error = Env->ParentNs.Enter();
        if (error) {
            L() << "Cannot enter namespaces: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        int cloneFlags = SIGCHLD;
        if (Env->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWIPC;

        if (Env->NewMountNs)
            cloneFlags |= CLONE_NEWNS;

        if (!Env->Hostname.empty())
            cloneFlags |= CLONE_NEWUTS;

        if (Env->NetCfg.NewNetNs)
            cloneFlags |= CLONE_NEWNET;

        int ret = pipe2(syncfd, O_CLOEXEC);
        if (ret) {
            TError error(EError::Unknown, errno, "pipe2(pdf)");
            L() << "Can't create sync pipe for child: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        WaitParentRfd = syncfd[0];
        WaitParentWfd = syncfd[1];

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);
        close(WaitParentRfd);
        ReportPid(clonePid);
        if (clonePid < 0) {
            TError error(errno == ENOMEM ?
                         EError::ResourceNotAvailable :
                         EError::Unknown, errno, "clone()");
            L() << "Can't spawn child: " << error << std::endl;
            Abort(error);
        }

        if (config().network().enabled()) {
            error = IsolateNet(clonePid);
            if (error) {
                L() << "Can't isolate child network: " << error << std::endl;
                Abort(error);
            }
        }

        int result = 0;
        ret = write(WaitParentWfd, &result, sizeof(result));
        if (ret != sizeof(result)) {
            TError error(EError::Unknown, "Partial write to child sync pipe (" + std::to_string(ret) + " != " + std::to_string(result) + ")");
            L() << "Can't spawn child: " << error << std::endl;
            Abort(error);
        }

        _exit(EXIT_SUCCESS);
    }
    close(Wfd);
    int status = 0;
    int forkResult = waitpid(forkPid, &status, 0);
    if (forkResult < 0)
        (void)kill(forkPid, SIGKILL);

    int n = read(Rfd, &Pid, sizeof(Pid));
    if (n <= 0) {
        close(Rfd);
        return TError(EError::InvalidValue, errno, "Container couldn't start due to resource limits");
    }

    TError error;
    (void)TError::Deserialize(Rfd, error);
    close(Rfd);
    if (error || status) {
        if (Pid > 0) {
            (void)kill(Pid, SIGKILL);
            L_ACT() << "Kill partly constructed container " << Pid << ": " << strerror(errno) << std::endl;
        }
        Pid = 0;
        ExitStatus = -1;

        if (!error)
            error = TError(EError::InvalidValue, errno, "Container couldn't start due to resource limits (child terminated with " + std::to_string(status) + ")");

        return error;
    }

    State = Started;

    ClearEnv();

    return TError::Success();
}

int TTask::GetPid() const {
    return Pid;
}

bool TTask::IsRunning() const {
    return State == Started;
}

int TTask::GetExitStatus() const {
    return ExitStatus;
}

void TTask::Exit(int status) {
    ExitStatus = status;
    State = Stopped;
}

TError TTask::Kill(int signal) const {
    if (!Pid)
        throw "Tried to kill invalid process!";

    L_ACT() << "kill " << signal << " " << Pid << std::endl;

    int ret = kill(Pid, signal);
    if (ret != 0)
        return TError(EError::Unknown, errno, "kill(" + std::to_string(Pid) + ")");

    return TError::Success();
}

bool TTask::IsZombie() const {
    TFile f("/proc/" + std::to_string(Pid) + "/status");

    std::vector<std::string> lines;
    TError err = f.AsLines(lines);
    if (err)
        return false;

    for (auto &l : lines)
        if (l.compare(0, 7, "State:\t") == 0)
            return l.substr(7, 1) == "Z";

    return false;
}

bool TTask::HasCorrectParent() {
    pid_t ppid;
    TError error = GetPPid(ppid);
    if (error) {
        L() << "Can't get ppid of restored task: " << error << std::endl;
        return false;
    }

    if (ppid != getppid()) {
        L() << "Invalid ppid of restored task: " << ppid << " != " << getppid() << std::endl;
        return false;
    }

    return true;
}

bool TTask::HasCorrectFreezer() {
    // if task belongs to different freezer cgroup we don't
    // restore it since pids may have wrapped or previous kvs state
    // is too old
    map<string, string> cgmap;
    TError error = GetTaskCgroups(Pid, cgmap);
    if (error) {
        L() << "Can't read " << Pid << " cgroups of restored task: " << error << std::endl;
        return false;
    } else {
        auto cg = Env->LeafCgroups.at(freezerSubsystem);
        if (cg && cg->Relpath().ToString() != cgmap["freezer"]) {
            // if at this point task is zombie we don't have any cgroup info
            if (IsZombie())
                return true;

            L_WRN() << "Unexpected freezer cgroup of restored task  " << Pid << ": " << cg->Path() << " != " << cgmap["freezer"] << std::endl;
            Pid = 0;
            State = Stopped;
            return false;
        }
    }

    return true;
}

void TTask::Restore(int pid_) {
    ExitStatus = 0;
    Pid = pid_;
    State = Started;
}

TError TTask::FixCgroups() const {
    if (IsZombie())
        return TError::Success();

    map<string, string> cgmap;
    TError error = GetTaskCgroups(Pid, cgmap);
    if (error)
        return error;

    for (auto pair : cgmap) {
        auto subsys = TSubsystem::Get(pair.first);
        auto &path = pair.second;

        if (!subsys || Env->LeafCgroups.find(subsys) == Env->LeafCgroups.end()) {
            if (pair.first.find(',') != std::string::npos)
                continue;
            if (pair.first == "net_cls" && !config().network().enabled()) {
                if (path == "/")
                    continue;

                L_WRN() << "No network, disabled " << subsys->GetName() << ":" << path << std::endl;

                auto cg = subsys->GetRootCgroup();
                error = cg->Attach(Pid);
                if (error)
                    L_ERR() << "Can't reattach to root: " << error << std::endl;
                continue;
            }

            error = TError(EError::Unknown, "Task belongs to unknown subsystem " + pair.first);
            L_WRN() << "Skip " << pair.first << ": " << error << std::endl;
            continue;
        }

        auto cg = Env->LeafCgroups.at(subsys);
        if (cg && cg->Relpath().ToString() != path) {
            L_WRN() << "Fixed invalid task subsystem for " << subsys->GetName() << ":" << path << std::endl;

            error = cg->Attach(Pid);
            if (error)
                L_ERR() << "Can't fix: " << error << std::endl;
        }
    }

    return TError::Success();
}

TError TTask::GetPPid(pid_t &ppid) const {
    TFile f("/proc/" + std::to_string(Pid) + "/status");

    std::vector<std::string> lines;
    TError err = f.AsLines(lines);
    if (err)
        return err;

    for (auto &l : lines)
        if (l.compare(0, 6, "PPid:\t") == 0)
            return StringToInt(l.substr(6), ppid);

    L_WRN() << "Can't parse /proc/pid/status" << std::endl;

    return TError(EError::Unknown, "Can't parse /proc/pid/status");
}

void TTask::ClearEnv() {
    Env = nullptr;
}

TError TaskGetLastCap() {
    TFile f("/proc/sys/kernel/cap_last_cap");
    return f.AsInt(lastCap);
}
