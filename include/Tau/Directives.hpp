/**
 * @file Directives.hpp
 * @brief All directive structs for the tau manifest DSL.
 *
 * Each directive maps 1-to-1 to a YAML key in a manifest list.
 * Directives are parsed from a NodeBase tree (via Manifest.hpp) into
 * typed structs consumed by the Runner.
 *
 * Naming convention:
 *   - struct D<Name> — data-only directive (no children)
 *   - struct D<Name>Block — directive with a child block of directives
 */

#pragma once

#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Forward declarations ─────────────────────────────────────────────────────
struct Directive;
using DirectiveList = Array<Directive *>;

// ─── Directive kind enum ──────────────────────────────────────────────────────
enum class DirectiveKind {
    // Timing
    Wait,
    WaitExit,
    Retry,

    // Execution
    Spawn,
    Dotenv,

    // Isolation
    Memory,
    CPUSet,
    CPU,
    Chroot,
    Isolate,
    Veth,

    IP,
    NewNet,
    Forward,
    Macvlan,
    IPVlan,
    Bridge,

    // Filesystem

    Mount,
    Copy,
    Unlink,
    Mkdir,
    Symlink,
    Image,
    Docker,
    VM,

    // Dependencies
    Local,
    Git,
    GHRelease,
    Manifest,   // manifest: path  (include)

    // Logic
    Or,
    And,
    Nand,
    Nor,
    Xor,

    // Variables
    Var,
    RegVar,
    RallVar,
    NewVar,
    RmVar,


    // Control
    Fail,
    Throw,

    // Metadata
    Name,
    Description,
    Version,
    Stars,
    Languages,
    Website,
    Issues,
    License,

    // Packaging
    Slot,
    Bin,
    BinDir,
};

// ─── Base directive ───────────────────────────────────────────────────────────
struct Directive {
    DirectiveKind kind;
    explicit Directive(DirectiveKind k) : kind(k) {}
    virtual ~Directive() = default;
};

// ─── Wait ─────────────────────────────────────────────────────────────────────
/// `- wait: 1.5s`
struct DWait : Directive {
    double seconds = 0.0;
    DWait() : Directive(DirectiveKind::Wait) {}
};

// ─── WaitExit ─────────────────────────────────────────────────────────────────
/// `- waitexit:` with a child block of directives
struct DWaitExit : Directive {
    DirectiveList children;
    DWaitExit() : Directive(DirectiveKind::WaitExit) {}
    ~DWaitExit() { for (auto *d : children) delete d; }
};

// ─── Retry ────────────────────────────────────────────────────────────────────
/// `- retry:`
struct DRetry : Directive {
    int           times   = 0;   ///< 0 = infinite
    double        backoff = 1.0; ///< seconds between retries
    DirectiveList children;
    DRetry() : Directive(DirectiveKind::Retry) {}
    ~DRetry() { for (auto *d : children) delete d; }
};

// ─── Dotenv ───────────────────────────────────────────────────────────────────
/// `- dotenv: ./path/to/.env`
struct DDotenv : Directive {
    String path;
    DDotenv() : Directive(DirectiveKind::Dotenv) {}
};

// ─── Spawn ────────────────────────────────────────────────────────────────────
/**
 * Simple form: `- spawn: ... command ...`
 * Explicit form:
 * ```yaml
 * - spawn:
 *     command: ... command ...
 *     wait: false
 *     name: myspawn
 * ```
 */
struct DSpawn : Directive {
    String command;
    bool   waitForExit = true;   ///< wait: true by default
    String name;                 ///< optional name; assigned numerically if empty
    pid_t  pid         = -1;
    int    exitCode    = -1;
    DSpawn() : Directive(DirectiveKind::Spawn) {}
};

// ─── Resource limits ─────────────────────────────────────────────────────────
/// `- memory: 15M` or `- memory_max: 50M`
struct DMemory : Directive {
    String raw;          ///< Baseline allocated memory, e.g. "15M", "1G"
    size_t bytes = 0;
    String rawMax;       ///< Maximum allowed memory (memory_max)
    size_t bytesMax = 0;
    DMemory() : Directive(DirectiveKind::Memory) {}
};

/// `- cpuset: 0,1,4`
struct DCPUSet : Directive {
    String cpus;   ///< cgroup cpuset string, e.g. "0,1,4"
    DCPUSet() : Directive(DirectiveKind::CPUSet) {}
};

/// `- cpu: 100`   (100 = 1 full core; cpucores*100 = unlimited)
struct DCPU : Directive {
    long long quota    = -1;  ///< Baseline allocation / weight; -1 = unlimited
    long long quotaMax = -1;  ///< Maximum quota (cpu_max); -1 = unlimited
    DCPU() : Directive(DirectiveKind::CPU) {}
};

// ─── Chroot ───────────────────────────────────────────────────────────────────
/// `- chroot: ./path/to/rootfs`
struct DChroot : Directive {
    String path;
    DChroot() : Directive(DirectiveKind::Chroot) {}
};

// ─── Isolate ──────────────────────────────────────────────────────────────────
/// `- isolate: true`
struct DIsolate : Directive {
    bool enable = true;
    DIsolate() : Directive(DirectiveKind::Isolate) {}
};


// ─── Networking ───────────────────────────────────────────────────────────────
/**
 * `- veth:`
 * `    name: veth_host`
 * `    peer: veth_container`
 */
struct DVeth : Directive {
    String hostName;
    String peerName;
    DVeth() : Directive(DirectiveKind::Veth) {}
};

/// `- ip: command`  (executes ip(8) in current namespace)
struct DIP : Directive {
    String command;
    DIP() : Directive(DirectiveKind::IP) {}
};

/// `- newnet: veth_container,exclude-list`
struct DNewNet : Directive {
    Array<String> allowList;  ///< interfaces to preserve (comma-separated in YAML)
    DNewNet() : Directive(DirectiveKind::NewNet) {}
};

/**
 * `- forward:`
 * `    target: veth_container`
 * `    source: eth0`
 * `    port: 1000-2000`
 * `    source-port: 1000`
 * `    source-ip: 192.168.1.*`
 * `    ip: 172.20.0.2`
 * `    protocol: tcp|udp|both`
 */
struct DForward : Directive {
    String target;
    String source;
    String port;
    String sourcePort;
    String sourceIP = "*";
    String ip;
    String protocol = "both";
    DForward() : Directive(DirectiveKind::Forward) {}
};

/**
 * `- macvlan:`
 * `    parent: eth0`
 * `    name: mac0`
 * `    mode: bridge|vepa|passthru|private`
 * `    mac: "52:54:00:12:34:56"`
 */
struct DMacvlan : Directive {
    String parent;
    String name;
    String mode = "bridge";
    String mac;
    DMacvlan() : Directive(DirectiveKind::Macvlan) {}
};

/**
 * `- ipvlan:`
 * `    parent: eth0`
 * `    name: ip0`
 * `    mode: l2|l3`
 */
struct DIPVlan : Directive {
    String parent;
    String name;
    String mode = "l2";
    DIPVlan() : Directive(DirectiveKind::IPVlan) {}
};

/**
 * `- bridge:`
 * `    source: br0`
 * `    target: veth_host`
 * `    address: 10.10.0.1/24`
 */
struct DBridge : Directive {
    String source;   ///< bridge name (e.g. br0)
    String target;   ///< interface to attach (e.g. veth_cont)
    String address;  ///< optional bridge IP/CIDR (e.g. 10.10.0.1/24)

    // Backward compatibility aliases
    String name;
    String attach;

    DBridge() : Directive(DirectiveKind::Bridge) {}
};


// ─── Filesystem operations ────────────────────────────────────────────────────
/**
 * `- mount:`
 * `    source: ..`
 * `    work: ..`    # if present → overlay; absent → bind
 * `    target: ..`
 * `    read: true`
 * `    write: true`
 */
struct DMount : Directive {
    String source;
    String work;    ///< overlay workdir; empty → bind mount
    String target;
    bool   read  = true;
    bool   write = true;
    DMount() : Directive(DirectiveKind::Mount) {}
};

/// `- copy:` with source and target
struct DCopy : Directive {
    String source;
    String target;
    DCopy() : Directive(DirectiveKind::Copy) {}
};

/// `- unlink: path`
struct DUnlink : Directive {
    String path;
    DUnlink() : Directive(DirectiveKind::Unlink) {}
};

/// `- mkdir: path/path`  (recursive)
struct DMkdir : Directive {
    String path;
    DMkdir() : Directive(DirectiveKind::Mkdir) {}
};

/// `- symlink:` with source and target
struct DSymlink : Directive {
    String source;
    String target;
    DSymlink() : Directive(DirectiveKind::Symlink) {}
};

// ─── Image ────────────────────────────────────────────────────────────────────
struct ImagePartition {
    String name;      ///< partition name/partlabel (e.g. "root", "boot", "esp")
    String uuid;      ///< partition UUID / PARTUUID
    String size;      ///< partition size (e.g. "512M", "10G", "max", "100%")
    String offset;    ///< optional partition start offset (e.g. "1M", "2048s")
    String type;      ///< filesystem type: ext4, ext3, ext2, xfs, btrfs, vfat, fat32, ntfs, f2fs, swap, raw
    String fstype;    ///< alias for type
    String label;     ///< filesystem volume label
    String flags;     ///< partition flags: boot, esp, bios_grub, root, lvm, raid, etc.
    int    number = 0;///< partition number (1-based, e.g. 1, 2, ...)

    String upper;     ///< overlayfs upper source (renamed from source)
    String work;      ///< overlayfs work dir — @ resolves inside image scope
    String lower;     ///< read-only base layer
    String target;    ///< where this partition is mounted
    bool   read  = true;
    bool   write = true;
    String source;    ///< backward compatibility alias for upper
};

/**
 * `- image:`
 * `    source: /path/to/disk.img`  # image file path on disk (renamed from path)
 * `    size: 100M`                 # disk size (supports auto-resizing)
 * `    type: ext4`                 # ext4, ext3, ext2, xfs, btrfs, vfat, fat32, ntfs, f2fs, squashfs, erofs, gpt, mbr
 * `    table: gpt`                 # partition table if partitioned: gpt, mbr/dos
 * `    label: MY_DISK`             # filesystem/disk label
 * `    create: true`
 * `    resize: true`               # auto-resize partition & fs on image growth
 * `    fsck: true`                 # integrity check before resize/mount
 * `    upper: /path/to/upper`      # optional: overlayfs upper source (renamed from source)
 * `    target: /tmp/myroot`        # optional: mount target (if omitted, only applies settings/resizes)
 * `    partitions:`                # optional partition list
 */
struct DImage : Directive {
    String source;      ///< image file path on disk (or raw block device)
    String size;        ///< disk image initial allocated size (e.g. "20G", "500M")
    String sizeMax;     ///< disk image maximum auto-expand size (size_max)
    String type;        ///< filesystem or partition table: ext4, ext3, ext2, xfs, btrfs, vfat, fat32, ntfs, f2fs, squashfs, erofs, gpt, mbr/dos, raw, qcow2
    String table;       ///< partition table format if partitioned: "gpt" or "mbr"/"dos"
    String format;      ///< disk format: raw, qcow2, vmdk, vdi
    String label;       ///< filesystem / disk label
    bool   create = true; ///< create image file if not existing
    bool   resize = true; ///< auto-resize partition/fs when size increases
    bool   fsck   = true; ///< run fsck before resize/mount

    // @ is valid in these fields — resolves to the image's internal temp base dir
    String upper;       ///< optional: initial content source / overlay upper (renamed from source)
    String work;        ///< overlayfs work dir (auto-derived from internal temp dir if empty)
    String lower;       ///< read-only base layer (e.g. a rootfs)
    String target;      ///< optional: where the merged usable root is placed
    bool   read  = true;
    bool   write = true;

    String path;        ///< backward compatibility alias for source

    Array<ImagePartition> partitions;  ///< populated when partitioned (gpt or mbr)

    DImage() : Directive(DirectiveKind::Image) {}
};

/**
 * `- docker:`
 * `    image: alpine:latest`       # e.g. "alpine", "library/ubuntu:22.04", "ghcr.io/..."
 * `    target: %5`                 # target mount directory
 * `    source: /tmp/upper`         # optional upperdir for read-write overlay
 * `    work: /tmp/work`            # optional workdir for read-write overlay
 * `    read: true`
 * `    write: true`
 */
struct DDocker : Directive {
    String image;
    String target;
    String source;
    String work;
    bool   read  = true;
    bool   write = true;

    DDocker() : Directive(DirectiveKind::Docker) {}
};

/**
 * `- vm:`
 * `    drives:`
 * `      - ./path/to/boot.img`
 * `      - source: ./path/to/extra.qcow2`
 * `        read: true`
 * `        write: true`
 * `        format: auto # default, or raw or qcow.`
 */
struct VMDrive {
    String source;
    String format = "auto";
    bool   read   = true;
    bool   write  = true;
};

struct DVM : Directive {
    Array<VMDrive> drives;
    String         kernel;
    String         initrd;
    String         cmdline;
    String         hypervisor  = "qemu";
    String         accel       = "kvm";
    bool           waitForExit = true;
    String         name;
    pid_t          pid         = -1;
    int            exitCode    = -1;
    DVM() : Directive(DirectiveKind::VM) {}
};

// ─── Dependencies ─────────────────────────────────────────────────────────────
/// `- local:` — copies source into store, runs like a git manifest
struct DLocal : Directive {
    String source;
    String target;
    bool   store = true;
    DLocal() : Directive(DirectiveKind::Local) {}
};


/**
 * `- git:`
 * `    source: https://github.com/user/repo.git`
 * `    branch: main`       # or ">v1.0.0" or "reg abc.*"
 * `    target: ./deps/...`
 * `    store: true`
 */
struct DGit : Directive {
    String source;
    String branch;    ///< raw branch spec (may include operator prefix)
    String target;
    bool   store = true;
    DGit() : Directive(DirectiveKind::Git) {}
};

/**
 * `- ghrelease:`
 * `    source: https://github.com/user/repo.git`
 * `    branch: >v1.0.0`   # tag matcher
 * `    name: Artifact.jar`
 * `    target: ./`
 */
struct DGHRelease : Directive {
    String source;
    String branch;   ///< tag matcher (same operator syntax as git:)
    String name;     ///< artifact name / "source" for repo snapshot
    String target;
    DGHRelease() : Directive(DirectiveKind::GHRelease) {}
};

/// `- manifest: path/to/another.yaml`
struct DManifest : Directive {
    String path;
    DManifest() : Directive(DirectiveKind::Manifest) {}
};

// ─── Logic blocks ─────────────────────────────────────────────────────────────
struct DLogicBlock : Directive {
    DirectiveList children;
    explicit DLogicBlock(DirectiveKind k) : Directive(k) {}
    ~DLogicBlock() { for (auto *d : children) delete d; }
};

struct DOr   : DLogicBlock { DOr()   : DLogicBlock(DirectiveKind::Or)   {} };
struct DAnd  : DLogicBlock { DAnd()  : DLogicBlock(DirectiveKind::And)  {} };
struct DNand : DLogicBlock { DNand() : DLogicBlock(DirectiveKind::Nand) {} };
struct DNor  : DLogicBlock { DNor()  : DLogicBlock(DirectiveKind::Nor)  {} };
struct DXor  : DLogicBlock { DXor()  : DLogicBlock(DirectiveKind::Xor)  {} };

// ─── Variables ────────────────────────────────────────────────────────────────
enum class VarOp {
    Assign,         ///< VAR = value
    CheckEquals,    ///< VAR == value
    CheckIncludes,  ///< VAR includes substring
    CheckRegex,     ///< VAR reg pattern
    CheckGT,        ///< VAR > value
    CheckGTE,       ///< VAR >= value
    CheckLT,        ///< VAR < value
    CheckLTE,       ///< VAR <= value
};

struct DVarBase : Directive {
    String key;        ///< key pattern (may be regex for regvar/rallvar)
    VarOp  op;
    String value;
    bool   keyIsRegex  = false;  ///< true for regvar:
    bool   matchAll    = false;  ///< true for rallvar:
    explicit DVarBase(DirectiveKind k) : Directive(k), op(VarOp::Assign) {}
};

struct DVar     : DVarBase { DVar()     : DVarBase(DirectiveKind::Var)     {} };
struct DRegVar  : DVarBase { DRegVar()  : DVarBase(DirectiveKind::RegVar)  { keyIsRegex = true; } };
struct DRallVar : DVarBase { DRallVar() : DVarBase(DirectiveKind::RallVar) { keyIsRegex = true; matchAll = true; } };

/// `- newvar: pattern` or `- newvar: [p1, p2]`
struct DNewVar : Directive {
    Array<String> patterns;
    DNewVar() : Directive(DirectiveKind::NewVar) {}
};

/// `- rmvar: pattern` or `- rmvar: [p1, p2]`
struct DRmVar : Directive {
    Array<String> patterns;
    DRmVar() : Directive(DirectiveKind::RmVar) {}
};


// ─── Control ──────────────────────────────────────────────────────────────────
/// `- fail: message`
struct DFail : Directive {
    String message;
    DFail() : Directive(DirectiveKind::Fail) {}
};

/// `- throw: message`  — propagates to top immediately
struct DThrow : Directive {
    String message;
    DThrow() : Directive(DirectiveKind::Throw) {}
};

// ─── Metadata directives ──────────────────────────────────────────────────────
struct DMetaString : Directive {
    String value;
    explicit DMetaString(DirectiveKind k) : Directive(k) {}
};

struct DName        : DMetaString { DName()        : DMetaString(DirectiveKind::Name)        {} };
struct DDescription : DMetaString { DDescription() : DMetaString(DirectiveKind::Description) {} };
struct DVersion     : DMetaString { DVersion()     : DMetaString(DirectiveKind::Version)     {} };
struct DWebsite     : DMetaString { DWebsite()     : DMetaString(DirectiveKind::Website)     {} };
struct DIssues      : DMetaString { DIssues()      : DMetaString(DirectiveKind::Issues)      {} };
struct DLicense     : DMetaString { DLicense()     : DMetaString(DirectiveKind::License)     {} };
struct DStars       : DMetaString { DStars()       : DMetaString(DirectiveKind::Stars)       {} };
struct DLanguages   : DMetaString { DLanguages()   : DMetaString(DirectiveKind::Languages)   {} };

// ─── Packaging ────────────────────────────────────────────────────────────────
/// `- slot: true`
struct DSlot : Directive {
    DSlot() : Directive(DirectiveKind::Slot) {}
};

/**
 * `- bin:`
 * `    name: myapp`
 * `    entry:`
 * `        - spawn: ...`
 * `    description: ...`
 * `    icon: ...`
 */
struct DBin : Directive {
    String       name;
    String       description;
    String       icon;
    DirectiveList entry;
    DBin() : Directive(DirectiveKind::Bin) {}
    ~DBin() { for (auto *d : entry) delete d; }
};

/// `- bindir: ./bin`
struct DBinDir : Directive {
    String path;
    DBinDir() : Directive(DirectiveKind::BinDir) {}
};

} // namespace Tau
