"""Fakes several test folders share: a gflib3 memory resource, the Pokemon system (language, region,
random numbers), and gflib3's file reading. Tests import them with decomp/tests on sys.path."""
import random
import struct

GET_TLS_VALUE = "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE"


class FakeResource:
    """A gfl::mem::MemoryResource whose allocations come from the emulator's heap (filled with 0xCC
    so missing initialization shows), and the thread's resource stack it gets pushed on."""

    def __init__(self, emu):
        self.log = []
        self.emu = emu

        def allocate(e):
            size, align = e.reg(1), e.reg(2)
            self.log.append(("allocate", size, align))
            return e.alloc(b"\xcc" * max(size, 1), align=max(align, 16))

        def deallocate(e):                                  # the address too: frees in another order differ
            self.log.append(("deallocate", e.reg(1), e.reg(2), e.reg(3)))

        def dump(e):
            self.log.append(("dump",))

        slots = [emu.host_function("resource dtor", lambda e: None), emu.host_function("resource dtor", lambda e: None),
                 emu.host_function("resource allocate", allocate), emu.host_function("resource deallocate", deallocate),
                 emu.host_function("resource is_equal", lambda e: 0)]
        slots += [emu.host_function("resource slot %d" % i, lambda e: 0) for i in range(5, 18)]
        slots += [emu.host_function("resource dump", dump)]
        vtable = emu.alloc(struct.pack("<%dQ" % len(slots), *slots))
        self.addr = emu.alloc(struct.pack("<QQ", vtable, 0))
        self.stack = emu.alloc(0x88)
        emu.import_handlers[GET_TLS_VALUE] = lambda e: self.stack

    def push(self):
        """Makes this the current resource, as it is while something is constructed in it."""
        e = self.emu
        count = e.read_u64(self.stack + 0x80)
        e.write(self.stack + 8 * count, struct.pack("<Q", self.addr))
        e.write(self.stack + 0x80, struct.pack("<Q", count + 1))

    def state(self):
        """What a test checks: the allocations and frees so far, and the resource stack."""
        return tuple(self.log), self.emu.read(self.stack, 0x88)


GET_SYSTEM = 0x710073B320


class FakeSystem:
    """pokemon::GetSystem's object (`addr`, or a new one GetSystem returns): the game's language and
    the console's region (randomize() picks new ones), and its random number generator, seeded the
    same in both runs. With shiny_id set, a third of the 32-bit numbers are PIDs that are shiny for
    that trainer ID."""

    def __init__(self, emu, addr=None, seed=1):
        rng = random.Random(seed)
        self.shiny_id = None
        self.emu = emu

        def random32(e):
            if self.shiny_id is not None and rng.random() < 0.33:
                low = rng.getrandbits(16)
                tid = self.shiny_id
                return ((tid >> 16 ^ tid ^ low ^ rng.getrandbits(4)) & 0xFFFF) << 16 | low
            return rng.getrandbits(32)

        def random_below(e):
            limit = e.reg(0) & 0xFFFFFFFF
            return rng.randrange(limit) if limit else 0

        self.addr = addr if addr is not None else emu.alloc(0x400)
        emu.write(self.addr + 0x310, struct.pack("<QQ", emu.host_function("Random()", random32),
                                                 emu.host_function("Random(max)", random_below)))
        if addr is None:
            emu.stub_function(GET_SYSTEM, "GetSystem", lambda e: self.addr)

    def randomize(self, rng):
        self.emu.write(self.addr + 0x308, bytes(rng.randrange(256) for _ in range(5)))


class FakeRefs:
    """Objects with gflib3's RefCounted interface at +0x48, named: AddRef and Release are logged
    with the object's name."""

    def __init__(self, emu, log):
        self.emu, self.log, self.names = emu, log, {}
        addref = emu.host_function("AddRef", lambda e: log.append(("addref", self.name_of(e.reg(0) - 0x48))))
        release = emu.host_function("Release", lambda e: log.append(("release", self.name_of(e.reg(0) - 0x48))))
        self.vtable = emu.alloc(struct.pack("<4Q", 0, 0, addref, release))

    def make(self, name, size=0x58):
        obj = self.emu.alloc(b"\0" * size)
        self.emu.write(obj + 0x48, struct.pack("<Q", self.vtable))
        self.names[obj] = name
        return obj

    def name_of(self, obj):
        return self.names.get(obj, "unknown")


READ_FILE, REMOVE_REQUEST = 0x71000ADC70, 0x7100023C70


class FakeFiles:
    """gflib3's file reading. A path builder stubbed with path() names its path; ReadFile gives the
    request a file with files[name] (an empty one when there's none); both and RemoveRequest are
    logged in `log` with the request's contents (but no stack addresses, which differ by version)."""

    def __init__(self, emu):
        self.emu, self.log, self.files = emu, [], {}
        self.refs = FakeRefs(emu, self.log)

        def read_file(e):
            request, path = e.reg(0), e.reg(1)
            name = self.refs.name_of(e.read_u64(path))
            data = self.files.get(name)
            f = self.refs.make("file " + name, 0x180)
            if data is not None:
                e.write(f + 0x160, struct.pack("<Q", len(data)))
                e.write(f + 0x170, struct.pack("<Q", e.alloc(data or b"\0")))
            self.log.append(("read", name, e.reg(2) & 0xFFFFFFFF, e.reg(3), e.reg(4), e.read(request, 0x110)))
            e.write(request + 8, struct.pack("<Q", f))
            return 1

        def remove_request(e):
            f, request = e.reg(0), e.reg(1)
            self.log.append(("remove", self.refs.name_of(f), e.read_u64(request), e.read_u64(request + 8) == f))

        emu.stub_function(READ_FILE, "gfl::fs::ReadFile", read_file)
        emu.stub_function(REMOVE_REQUEST, "gfl::fs::File::RemoveRequest", remove_request)

    def path(self, addr, name):
        """Stubs a function that returns a path (in X8); name(emu) names the file it points to."""
        def build(e):
            text, extra = self.refs.make(name(e)), self.refs.make("path extra")
            e.write(e.reg(8), struct.pack("<5QI", text, 0x1111, 0x2222, 0x3333, extra, 0))     # all 0x2C bytes
        self.emu.stub_function(addr, "path builder 0x%x" % addr, build)
