## Early Service Example

When working in some industries with strict boot time requirements, booting
all the way to the root filesystem is not quick enough to meet the timing
requirements for some use cases. This project shows how to start a
short-lived service in the initial ramdisk (initrd), and pass the running
state to a long-lived service that is started from the root filesystem.

The [example program](early-service.c) has two parts: 1) a timer that
periodically prints and increments a counter, and 2) a server that listens
for commands on a UNIX domain socket. This is wired together like so:

- The [early-service-initrd.service](conf/early-service-initrd.service) systemd
  unit is started inside the initrd and exposes the short-lived server at the
  UNIX domain socket `/run/early-service-initrd/early-service.sock`. The
  contents of the `/run` directory in the initrd are also exposed to `/run` on
  the root filesystem.

- Later when systemd is started from the root filesystem,
  [early-service.service](conf/early-service.service) is started. This exposes
  the long-running server at the UNIX domain socket
  `/run/early-service/early-service.sock`. Separately, it is also told to read
  the current state of the version running in the initrd from the UNIX domain
  socket `/run/early-service-initrd/early-service.sock`. Once the state is
  passed to the version running on the root filesystem, the process that was
  started from the initrd is told to terminate.

- There is a [dracut module](conf/module-setup.sh) that will tell dracut to
  include the necessary files in the initrd.

The following annotated log output from `journalctl` shows the two processes
starting, and exchanging state:

    # early-service-initrd.service is started inside the initrd.
    Mar 28 10:39:17 localhost early-service[324]: Listening on UNIX socket /run/early-service-initrd/early-service.sock
    Mar 28 10:39:18 localhost early-service[324]: 0
    Mar 28 10:39:18 localhost early-service[324]: 1
    Mar 28 10:39:18 localhost early-service-initrd[324]: ** Message: 10:39:18.223: 2
    Mar 28 10:39:18 localhost early-service-initrd[324]: ** Message: 10:39:18.323: 3
    Mar 28 10:39:18 localhost early-service-initrd[324]: ** Message: 10:39:18.424: 4
    Mar 28 10:39:18 localhost early-service-initrd[324]: ** Message: 10:39:18.524: 5
    # early-service.service is started from the root filesystem; reads state from running initrd process.
    Mar 28 10:39:18 localhost early-service[432]: Reading starting position from socket /run/early-service-initrd/early-service.sock
    Mar 28 10:39:18 localhost early-service-initrd[324]: ** Message: 10:39:18.539: Returning counter to client and terminating the process
    Mar 28 10:39:18 localhost early-service[432]: Listening on UNIX socket /run/early-service/early-service.sock
    Mar 28 10:39:18 localhost systemd[1]: early-service-initrd.service: Deactivated successfully.
    Mar 28 10:39:18 localhost early-service[432]: 6
    Mar 28 10:39:18 localhost early-service[432]: 7
    # Only the version started from the root filesystem is now running.

It's intended that you will have some minimal service that runs in the initrd
that does as little as possible, and passes it's state to the fully featured
services running from the root filesystem. The initrd version should only be
running for a few seconds at most.


## Commands available on UNIX domain socket

The following commands are available over the UNIX domain socket at
`/run/early-service/early-service.sock`:

- `get_counter`
- `get_counter_and_terminate`
- `set_counter ###`

You can test the API by using Netcat:

    $ sudo dnf install nc
    $ echo "set_counter 100" | sudo nc -U /run/early-service/early-service.sock
    previous value 502
    $ echo "get_counter" | sudo nc -U /run/early-service/early-service.sock
    137
    $ echo "get_counter" | sudo nc -U /run/early-service/early-service.sock
    141


## Why not start long running services from the initrd?

Here's some reasons why you don't want to have long-running, fully featured
services started in the initrd:

- You can't leak references to any files on the initrd, otherwise the kernel
  won't be able to free that memory when the initrd is unmounted.

- The initrd is a cpio archive, and increasing the size of the initrd is going
  to increase the kernel boot time since it will need to uncompress and extract
  the cpio archive.

- Any services started from the initrd will be started before the SELinux
  policy is loaded. Services started from the initrd will run with the
  `kernel_t` label.

- Services started from the initrd can't depend on almost anything like mounts,
  devices, services, dbus, etc so it's difficult to develop software of any
  complexity.

- Adding all of these dependencies to the initrd is only going to move the
  timing bottlenecks booting from the root filesystem to the initrd.


## Integrating this into CentOS Automotive Sample Images

You can use the following commands to build a RPM, and incorporate that
RPM into an image created by the
[Automotive Sample Images](https://gitlab.com/CentOS/automotive/sample-images).

    $ scripts/create-rpm.sh early-service.spec
    $ createrepo_c ~/rpmbuild/RPMS/x86_64/
    $ cd /path/to/sample-images/osbuild-manifests
    $ make DEFINES+='extra_repos=[{"id":"local","baseurl":"file:///home/masneyb/rpmbuild/RPMS/x86_64"}] extra_rpms=["early-service"] image_enabled_services=["early-service"]' cs9-qemu-developer-regular.x86_64.qcow2

If you are incrementally rebuilding the same RPM for testing, then you'll need
to run `dnf clean all` as your regular user to clean the dnf caches before
rerunning the `make` command.
