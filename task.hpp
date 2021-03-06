#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "util/namespace.hpp"
#include "util/path.hpp"
#include "util/netlink.hpp"
#include "util/cred.hpp"

extern "C" {
#include <sys/resource.h>
}

class TTask;
class TContainerEnv;
class TFolder;
class TCgroup;
class TSubsystem;

struct TExitStatus {
    // Task was not started due to the following error
    int Error;
    // Task exited with given status
    int Status;
};

struct TBindMap {
    TPath Source;
    TPath Dest;
    bool Rdonly;
};

struct THostNetCfg {
    std::string Dev;
};

struct TMacVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Type;
    std::string Hw;
    int Mtu;
};

struct TIpVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Mode;
    int Mtu;
};

struct TIpVec {
    std::string Iface;
    TNlAddr Addr;
    int Prefix;
};

struct TGwVec {
    std::string Iface;
    TNlAddr Addr;
};

struct TVethNetCfg {
    std::string Bridge;
    std::string Name;
    std::string Hw;
    std::string Peer;
    int Mtu;
};

struct TNetCfg {
    bool NewNetNs;
    bool Inherited;
    bool Host;
    std::vector<THostNetCfg> HostIface;
    std::vector<TMacVlanNetCfg> MacVlan;
    std::vector<TIpVlanNetCfg> IpVlan;
    std::vector<TVethNetCfg> Veth;
    std::string NetNsName;

    void Clear() {
        /* default - create new empty netns */
        NewNetNs = true;
        Host = false;
        Inherited = false;
        HostIface.clear();
        MacVlan.clear();
        IpVlan.clear();
        Veth.clear();
        NetNsName = "";
    }
};

struct TTaskEnv : public TNonCopyable {
    std::string Command;
    TPath Cwd;
    bool CreateCwd;
    TPath Root;
    bool RootRdOnly;
    std::string User;
    std::string Group;
    std::vector<std::string> Environ;
    bool Isolate = false;
    TPath StdinPath;
    TPath StdoutPath;
    TPath StderrPath;
    TNamespaceSnapshot ParentNs;
    TNamespaceFd ClientMntNs;
    std::map<int,struct rlimit> Rlimit;
    std::string Hostname;
    bool BindDns;
    std::vector<TBindMap> BindMap;
    TNetCfg NetCfg;
    TPath Loop;
    int LoopDev;
    uint64_t Caps;
    std::vector<TGwVec> GwVec;
    std::vector<TIpVec> IpVec;
    bool NewMountNs;
    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    std::unique_ptr<TScopedMem> GroupList;
    TCred Cred;

    TError GetGroupList();
    TError Prepare(const TCred &cred);
    const char** GetEnvp() const;
    bool EnvHasKey(const std::string &key);
};

class TTask: public TNonCopyable {
    int Rfd, Wfd;
    int WaitParentRfd, WaitParentWfd;
    std::shared_ptr<const TTaskEnv> Env;

    enum ETaskState { Stopped, Started } State;
    int ExitStatus;

    pid_t Pid;
    std::shared_ptr<TFolder> Cwd;

    void ReportPid(int pid) const;

    TError ReopenStdio();
    TError IsolateNet(int childPid);
    TError CreateCwd();

    TError ChildCreateNode(const TPath &path, unsigned int mode, unsigned int dev);
    TError ChildOpenStdFile(const TPath &path, int expected);
    TError ChildApplyCapabilities();
    TError ChildDropPriveleges();
    TError ChildExec();
    TError ChildBindDns();
    TError ChildBindDirectores();
    TError ChildRestrictProc(bool restrictProcSys);
    TError ChildMountRootFs();
    TError ChildMountDev();
    TError ChildMountRun();
    TError ChildRemountRootRo();
    TError ChildIsolateFs();
    TError ChildEnableNet();

public:
    TTask(std::shared_ptr<const TTaskEnv> env) : Env(env) {};
    TTask(pid_t pid) : Pid(pid) {};

    TError Start();
    int GetPid() const;
    bool IsRunning() const;
    int GetExitStatus() const;
    TError Kill(int signal) const;
    void Exit(int status);
    void ClearEnv();

    TError ChildApplyLimits();
    TError ChildSetHostname();
    TError ChildPrepareLoop();
    TError ChildCallback();
    void Restore(int pid_);
    TError FixCgroups() const;
    void Abort(const TError &error) const;

    TError GetPPid(pid_t &ppid) const;
    bool IsZombie() const;

    bool HasCorrectParent();
    bool HasCorrectFreezer();

    std::shared_ptr<TFolder> StdTmp;
    TError CreateTmpDir(const TPath &path, std::shared_ptr<TFolder> &dir) const;
};

TError TaskGetLastCap();
