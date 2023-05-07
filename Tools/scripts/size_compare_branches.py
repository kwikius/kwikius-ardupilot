#!/usr/bin/env python3

'''
Wrapper around elf_diff (https://github.com/noseglasses/elf_diff)
to create a html report comparing an ArduPilot build across two
branches

pip3 install --user elf_diff weasyprint

AP_FLAKE8_CLEAN

How to use?
Starting in the ardupilot directory.
~/ardupilot $ python Tools/scripts/size_compare_branches.py --branch=[PR_BRANCH_NAME] --vehicle=copter

Output is placed into ../ELF_DIFF_[VEHICLE_NAME]
'''

import copy
import optparse
import os
import shutil
import string
import subprocess
import sys
import tempfile
import threading
import time
import board_list

if sys.version_info[0] < 3:
    running_python3 = False
else:
    running_python3 = True


class SizeCompareBranchesResult(object):
    '''object to return results from a comparison'''

    def __init__(self, board, vehicle, bytes_delta, identical):
        self.board = board
        self.vehicle = vehicle
        self.bytes_delta = bytes_delta
        self.identical = identical


class SizeCompareBranches(object):
    '''script to build and compare branches using elf_diff'''

    def __init__(self,
                 branch=None,
                 master_branch="master",
                 board=["MatekF405-Wing"],
                 vehicle=["plane"],
                 bin_dir=None,
                 run_elf_diff=True,
                 all_vehicles=False,
                 all_boards=False,
                 use_merge_base=True,
                 waf_consistent_builds=True,
                 show_empty=True,
                 extra_hwdef=[],
                 extra_hwdef_branch=[],
                 extra_hwdef_master=[],
                 parallel_copies=None,
                 jobs=None):

        if branch is None:
            branch = self.find_current_git_branch_or_sha1()

        self.master_branch = master_branch
        self.branch = branch
        self.board = board
        self.vehicle = vehicle
        self.bin_dir = bin_dir
        self.run_elf_diff = run_elf_diff
        self.extra_hwdef = extra_hwdef
        self.extra_hwdef_branch = extra_hwdef_branch
        self.extra_hwdef_master = extra_hwdef_master
        self.all_vehicles = all_vehicles
        self.all_boards = all_boards
        self.use_merge_base = use_merge_base
        self.waf_consistent_builds = waf_consistent_builds
        self.show_empty = show_empty
        self.parallel_copies = parallel_copies
        self.jobs = jobs

        if self.bin_dir is None:
            self.bin_dir = self.find_bin_dir()

        self.boards_by_name = {}
        for board in board_list.BoardList().boards:
            self.boards_by_name[board.name] = board

        # map from vehicle names to binary names
        self.vehicle_map = {
            "rover"     : "ardurover",
            "copter"    : "arducopter",
            "plane"     : "arduplane",
            "sub"       : "ardusub",
            "heli"      : "arducopter-heli",
            "blimp"     : "blimp",
            "antennatracker" : "antennatracker",
            "AP_Periph" : "AP_Periph",
            "bootloader": "AP_Bootloader",
            "iofirmware": "iofirmware_highpolh",  # FIXME: lowpolh?
        }

        if all_boards:
            self.board = sorted(list(self.boards_by_name.keys()), key=lambda x: x.lower())
        else:
            # validate boards
            all_boards = set(self.boards_by_name.keys())
            for b in self.board:
                if b not in all_boards:
                    raise ValueError("Bad board %s" % str(b))

        if all_vehicles:
            self.vehicle = sorted(list(self.vehicle_map.keys()), key=lambda x: x.lower())
        else:
            for v in self.vehicle:
                if v not in self.vehicle_map.keys():
                    raise ValueError("Bad vehicle (%s); choose from %s" % (v, ",".join(self.vehicle_map.keys())))

        # some boards we don't have a -bl.dat for, so skip them.
        # TODO: find a way to get this information from board_list:
        self.bootloader_blacklist = set([
            'CubeOrange-SimOnHardWare',
            'CubeOrangePlus-SimOnHardWare',
            'fmuv2',
            'fmuv3-bdshot',
            'iomcu',
            'iomcu',
            'iomcu_f103_8MHz',
            'luminousbee4',
            'skyviper-v2450',
            'skyviper-f412-rev1',
            'skyviper-journey',
            'Pixhawk1-1M-bdshot',
            'SITL_arm_linux_gnueabihf',
        ])

        # blacklist all linux boards for bootloader build:
        self.bootloader_blacklist.update(self.linux_board_names())
        # ... and esp32 boards:
        self.bootloader_blacklist.update(self.esp32_board_names())

    def linux_board_names(self):
        '''return a list of all Linux board names; FIXME: get this dynamically'''
        # grep 'class.*[(]linux' Tools/ardupilotwaf/boards.py  | perl -pe "s/class (.*)\(linux\).*/            '\\1',/"
        return [
            'navigator',
            'erleboard',
            'navio',
            'navio2',
            'edge',
            'zynq',
            'ocpoc_zynq',
            'bbbmini',
            'blue',
            'pocket',
            'pxf',
            'bebop',
            'vnav',
            'disco',
            'erlebrain2',
            'bhat',
            'dark',
            'pxfmini',
            'aero',
            'rst_zynq',
            'obal',
            'SITL_x86_64_linux_gnu',
        ]

    def esp32_board_names(self):
        return [
            'esp32buzz',
            'esp32empty',
            'esp32tomte76',
            'esp32icarous',
            'esp32diy',
        ]

    def find_bin_dir(self):
        '''attempt to find where the arm-none-eabi tools are'''
        binary = shutil.which("arm-none-eabi-g++")
        if binary is None:
            raise Exception("No arm-none-eabi-g++?")
        return os.path.dirname(binary)

    # vast amounts of stuff copied into here from build_binaries.py

    def run_program(self, prefix, cmd_list, show_output=True, env=None, show_output_on_error=True, show_command=None, cwd="."):
        if show_command is None:
            show_command = True
        if show_command:
            cmd = " ".join(cmd_list)
            if cwd is None:
                cwd = "."
            self.progress(f"Running ({cmd}) in ({cwd})")
        p = subprocess.Popen(
            cmd_list,
            stdin=None,
            stdout=subprocess.PIPE,
            close_fds=True,
            stderr=subprocess.STDOUT,
            cwd=cwd,
            env=env)
        output = ""
        while True:
            x = p.stdout.readline()
            if len(x) == 0:
                returncode = os.waitpid(p.pid, 0)
                if returncode:
                    break
                    # select not available on Windows... probably...
                time.sleep(0.1)
                continue
            if running_python3:
                x = bytearray(x)
                x = filter(lambda x : chr(x) in string.printable, x)
                x = "".join([chr(c) for c in x])
            output += x
            x = x.rstrip()
            some_output = "%s: %s" % (prefix, x)
            if show_output:
                print(some_output)
            else:
                output += some_output
        (_, status) = returncode
        if status != 0:
            if not show_output and show_output_on_error:
                # we were told not to show output, but we just
                # failed... so show output...
                print(output)
            self.progress("Process failed (%s)" %
                          str(returncode))
            raise subprocess.CalledProcessError(
                returncode, cmd_list)
        return output

    def find_current_git_branch_or_sha1(self):
        try:
            output = self.run_git(["symbolic-ref", "--short", "HEAD"])
            output = output.strip()
            return output
        except subprocess.CalledProcessError:
            pass

        # probably in a detached-head state.  Get a sha1 instead:
        output = self.run_git(["rev-parse", "--short", "HEAD"])
        output = output.strip()
        return output

    def find_git_branch_merge_base(self, branch, master_branch):
        output = self.run_git(["merge-base", branch, master_branch])
        output = output.strip()
        return output

    def run_git(self, args, show_output=True, source_dir=None):
        '''run git with args git_args; returns git's output'''
        cmd_list = ["git"]
        cmd_list.extend(args)
        return self.run_program("SCB-GIT", cmd_list, show_output=show_output, cwd=source_dir)

    def run_waf(self, args, compiler=None, show_output=True, source_dir=None):
        # try to modify the environment so we can consistent builds:
        consistent_build_envs = {
            "CHIBIOS_GIT_VERSION": "12345678",
            "GIT_VERSION": "abcdef",
            "GIT_VERSION_INT": "15",
        }
        for (n, v) in consistent_build_envs.items():
            os.environ[n] = v

        if os.path.exists("waf"):
            waf = "./waf"
        else:
            waf = os.path.join(".", "modules", "waf", "waf-light")
        cmd_list = [waf]
        cmd_list.extend(args)
        env = None
        if compiler is not None:
            # default to $HOME/arm-gcc, but allow for any path with AP_GCC_HOME environment variable
            gcc_home = os.environ.get("AP_GCC_HOME", os.path.join(os.environ["HOME"], "arm-gcc"))
            gcc_path = os.path.join(gcc_home, compiler, "bin")
            if os.path.exists(gcc_path):
                # setup PATH to point at the right compiler, and setup to use ccache
                env = os.environ.copy()
                env["PATH"] = gcc_path + ":" + env["PATH"]
                env["CC"] = "ccache arm-none-eabi-gcc"
                env["CXX"] = "ccache arm-none-eabi-g++"
            else:
                raise Exception("BB-WAF: Missing compiler %s" % gcc_path)
        self.run_program("SCB-WAF", cmd_list, env=env, show_output=show_output, cwd=source_dir)

    def progress(self, string):
        '''pretty-print progress'''
        print("SCB: %s" % string)

    def build_branch_into_dir(self, board, branch, vehicle, outdir, source_dir=None, extra_hwdef=None, jobs=None):
        self.run_git(["checkout", branch], show_output=False, source_dir=source_dir)
        self.run_git(["submodule", "update", "--recursive"], show_output=False, source_dir=source_dir)
        build_dir = "build"
        if source_dir is not None:
            build_dir = os.path.join(source_dir, "build")
        shutil.rmtree(build_dir, ignore_errors=True)
        waf_configure_args = ["configure", "--board", board]
        if self.waf_consistent_builds:
            waf_configure_args.append("--consistent-builds")

        if extra_hwdef is not None:
            waf_configure_args.extend(["--extra-hwdef", extra_hwdef])

        if jobs is None:
            jobs = self.jobs
        if jobs is not None:
            waf_configure_args.extend(["-j", str(jobs)])

        self.run_waf(waf_configure_args, show_output=False, source_dir=source_dir)
        # we can't run `./waf copter blimp plane` without error, so do
        # them one-at-a-time:
        for v in vehicle:
            if v == 'bootloader':
                # need special configuration directive
                continue
            self.run_waf([v], show_output=False, source_dir=source_dir)
        for v in vehicle:
            if v != 'bootloader':
                continue
            if board in self.bootloader_blacklist:
                continue
            # need special configuration directive
            bootloader_waf_configure_args = copy.copy(waf_configure_args)
            bootloader_waf_configure_args.append('--bootloader')
            # hopefully temporary hack so you can build bootloader
            # after building other vehicles without a clean:
            dsdl_generated_path = os.path.join('build', board, "modules", "DroneCAN", "libcanard", "dsdlc_generated")
            self.progress("HACK: Removing (%s)" % dsdl_generated_path)
            if source_dir is not None:
                dsdl_generated_path = os.path.join(source_dir, dsdl_generated_path)
            shutil.rmtree(dsdl_generated_path, ignore_errors=True)
            self.run_waf(bootloader_waf_configure_args, show_output=False, source_dir=source_dir)
            self.run_waf([v], show_output=False, source_dir=source_dir)
        self.run_program("rsync", ["rsync", "-ap", "build/", outdir], cwd=source_dir)

    def vehicles_to_build_for_board_info(self, board_info):
        vehicles_to_build = []
        for vehicle in self.vehicle:
            if vehicle == 'AP_Periph':
                if not board_info.is_ap_periph:
                    continue
            else:
                if board_info.is_ap_periph:
                    continue
                # the bootloader target isn't an autobuild target, so
                # it gets special treatment here:
                if vehicle != 'bootloader' and vehicle.lower() not in [x.lower() for x in board_info.autobuild_targets]:
                    continue
            vehicles_to_build.append(vehicle)

        return vehicles_to_build

    def parallel_thread_main(self, thread_number):
        # initialisation; make a copy of the source directory
        my_source_dir = os.path.join(self.tmpdir, f"thread-{thread_number}-source")
        self.run_program("rsync", [
            "rsync",
            "--exclude=build/",
            "-ap",
            "./",
            my_source_dir
        ])

        while True:
            try:
                task = self.parallel_tasks.pop(0)
            except IndexError:
                break
            jobs = None
            if self.jobs is not None:
                jobs = int(self.jobs / self.num_threads_remaining)
                if jobs <= 0:
                    jobs = 1
            self.run_build_task(task, source_dir=my_source_dir, jobs=jobs)

    def run_build_tasks_in_parallel(self, tasks):
        n_threads = self.parallel_copies
        if len(tasks) < n_threads:
            n_threads = len(tasks)
        self.num_threads_remaining = n_threads

        # shared list for the threads:
        self.parallel_tasks = copy.copy(tasks)  # make this an argument instead?!
        threads = []
        for i in range(0, n_threads):
            t = threading.Thread(
                target=self.parallel_thread_main,
                name=f'task-builder-{i}',
                args=[i],
            )
            t.start()
            threads.append(t)
        tstart = time.time()
        while len(threads):
            new_threads = []
            for thread in threads:
                thread.join(0)
                if thread.is_alive():
                    new_threads.append(thread)
            threads = new_threads
            self.num_threads_remaining = len(threads)
            self.progress(f"remaining-tasks={len(self.parallel_tasks)} remaining-threads={len(threads)} elapsed={int(time.time() - tstart)}s")  # noqa

            # write out a progress CSV:
            task_results = []
            for task in tasks:
                task_results.append(self.gather_results_for_task(task))
            # progress CSV:
            with open("/tmp/some.csv", "w") as f:
                f.write(self.csv_for_results(self.compare_task_results(task_results, no_elf_diff=True)))

            time.sleep(1)
        self.progress("All threads returned")

    def run_all(self):
        '''run tests for boards and vehicles passed in constructor'''

        tmpdir = tempfile.mkdtemp()
        self.tmpdir = tmpdir

        self.master_commit = self.master_branch
        if self.use_merge_base:
            self.master_commit = self.find_git_branch_merge_base(self.branch, self.master_branch)
            self.progress("Using merge base (%s)" % self.master_commit)

        # create an array of tasks to run:
        tasks = []
        for board in self.board:
            board_info = self.boards_by_name[board]

            vehicles_to_build = self.vehicles_to_build_for_board_info(board_info)

            outdir_1 = os.path.join(tmpdir, "out-master-%s" % (board,))
            tasks.append((board, self.master_commit, outdir_1, vehicles_to_build, self.extra_hwdef_master))
            outdir_2 = os.path.join(tmpdir, "out-branch-%s" % (board,))
            tasks.append((board, self.branch, outdir_2, vehicles_to_build, self.extra_hwdef_branch))

        if self.parallel_copies is not None:
            self.run_build_tasks_in_parallel(tasks)
            task_results = []
            for task in tasks:
                task_results.append(self.gather_results_for_task(task))
        else:
            # traditional build everything in sequence:
            task_results = []
            for task in tasks:
                self.run_build_task(task)
                task_results.append(self.gather_results_for_task(task))

                # progress CSV:
                with open("/tmp/some.csv", "w") as f:
                    f.write(self.csv_for_results(self.compare_task_results(task_results, no_elf_diff=True)))

        return self.compare_task_results(task_results)

    def elf_diff_results(self, result_master, result_branch):
        master_branch = result_master["branch"]
        branch = result_master["branch"]
        for vehicle in result_master["vehicle"].keys():
            elf_filename = result_master["vehicle"][vehicle]["elf_filename"]
            master_elf_dir = result_master["vehicle"][vehicle]["elf_dir"]
            new_elf_dir = result_branch["vehicle"][vehicle]["elf_dir"]
            board = result_master["board"]
            self.progress("Starting compare (~10 minutes!)")
            elf_diff_commandline = [
                "time",
                "python3",
                "-m", "elf_diff",
                "--bin_dir", self.bin_dir,
                '--bin_prefix=arm-none-eabi-',
                "--old_alias", "%s %s" % (master_branch, elf_filename),
                "--new_alias", "%s %s" % (branch, elf_filename),
                "--html_dir", "../ELF_DIFF_%s_%s" % (board, vehicle),
                os.path.join(master_elf_dir, elf_filename),
                os.path.join(new_elf_dir, elf_filename)
            ]

            self.run_program("SCB", elf_diff_commandline)

    def compare_task_results(self, task_results, no_elf_diff=False):
        # pair off results, master and branch:
        pairs = {}
        for res in task_results:
            board = res["board"]
            if board not in pairs:
                pairs[board] = {}
            if res["branch"] == self.master_commit:
                pairs[board]["master"] = res
            elif res["branch"] == self.branch:
                pairs[board]["branch"] = res
            else:
                raise ValueError(res["branch"])

        results = {}
        for pair in pairs.values():
            if "master" not in pair or "branch" not in pair:
                # probably incomplete:
                continue
            master = pair["master"]
            board = master["board"]
            try:
                results[board] = self.compare_results(master, pair["branch"])
                if not no_elf_diff:
                    self.elf_diff_results(master, pair["branch"])
            except FileNotFoundError:
                pass

        return results

    def emit_csv_for_results(self, results):
        '''emit dictionary of dictionaries as a CSV'''
        print(self.csv_for_results(results))

    def csv_for_results(self, results):
        '''return a string with csv for results'''
        boards = sorted(results.keys())
        all_vehicles = set()
        for board in boards:
            all_vehicles.update(list(results[board].keys()))
        sorted_all_vehicles = sorted(list(all_vehicles))
        ret = ""
        ret += ",".join(["Board"] + sorted_all_vehicles) + "\n"
        for board in boards:
            line = [board]
            board_results = results[board]
            for vehicle in sorted_all_vehicles:
                bytes_delta = ""
                if vehicle in board_results:
                    result = board_results[vehicle]
                    if result.identical:
                        bytes_delta = "*"
                    else:
                        bytes_delta = result.bytes_delta
                line.append(str(bytes_delta))
            # do not add to ret value if we're not showing empty results:
            if not self.show_empty:
                if len(list(filter(lambda x : x != "", line[1:]))) == 0:
                    continue
            ret += ",".join(line) + "\n"
        return ret

    def run(self):
        results = self.run_all()
        self.emit_csv_for_results(results)

    def files_are_identical(self, file1, file2):
        '''returns true if the files have the same content'''
        return open(file1, "rb").read() == open(file2, "rb").read()

    def extra_hwdef_file(self, more):
        # create a combined list of hwdefs:
        extra_hwdefs = []
        extra_hwdefs.extend(self.extra_hwdef)
        extra_hwdefs.extend(more)
        extra_hwdefs = list(filter(lambda x : x is not None, extra_hwdefs))
        if len(extra_hwdefs) == 0:
            return None

        # slurp all content into a variable:
        content = bytearray()
        for extra_hwdef in extra_hwdefs:
            with open(extra_hwdef, "r+b") as f:
                content += f.read()

        # spew content to single file:
        f = tempfile.NamedTemporaryFile(delete=False)
        f.write(content)
        f.close()

        return f.name

    def run_build_task(self, task, source_dir=None, jobs=None):
        (board, commitish, outdir, vehicles_to_build, extra_hwdef_file) = task

        self.progress(f"Building {task}")
        shutil.rmtree(outdir, ignore_errors=True)
        self.build_branch_into_dir(
            board,
            commitish,
            vehicles_to_build,
            outdir,
            source_dir=source_dir,
            extra_hwdef=self.extra_hwdef_file(extra_hwdef_file),
            jobs=jobs,
        )

    def gather_results_for_task(self, task):
        (board, commitish, outdir, vehicles_to_build, extra_hwdef_file) = task

        result = {
            "board": board,
            "branch": commitish,
            "vehicle": {},
        }

        for vehicle in vehicles_to_build:
            if vehicle == 'bootloader' and board in self.bootloader_blacklist:
                continue

            result["vehicle"][vehicle] = {}
            v = result["vehicle"][vehicle]
            v["bin_filename"] = self.vehicle_map[vehicle] + '.bin'
            v["bin_dir"] = os.path.join(outdir, board, "bin")

            elf_dirname = "bin"
            if vehicle == 'bootloader':
                # elfs for bootloaders are in the bootloader directory...
                elf_dirname = "bootloader"
            elf_dir = os.path.join(outdir, board, elf_dirname)
            v["elf_dir"] = elf_dir
            v["elf_filename"] = self.vehicle_map[vehicle]

        return result

    def compare_results(self, result_master, result_branch):
        ret = {}
        for vehicle in result_master["vehicle"].keys():
            # check for the difference in size (and identicality)
            # of the two binaries:
            master_bin_dir = result_master["vehicle"][vehicle]["bin_dir"]
            new_bin_dir = result_branch["vehicle"][vehicle]["bin_dir"]

            try:
                bin_filename = result_master["vehicle"][vehicle]["bin_filename"]
                master_path = os.path.join(master_bin_dir, bin_filename)
                new_path = os.path.join(new_bin_dir, bin_filename)
                master_size = os.path.getsize(master_path)
                new_size = os.path.getsize(new_path)
            except FileNotFoundError:
                elf_filename = result_master["vehicle"][vehicle]["elf_filename"]
                master_path = os.path.join(master_bin_dir, elf_filename)
                new_path = os.path.join(new_bin_dir, elf_filename)
                master_size = os.path.getsize(master_path)
                new_size = os.path.getsize(new_path)

            identical = self.files_are_identical(master_path, new_path)

            board = result_master["board"]
            ret[vehicle] = SizeCompareBranchesResult(board, vehicle, new_size - master_size, identical)

        return ret


if __name__ == '__main__':
    parser = optparse.OptionParser("size_compare_branches.py")
    parser.add_option("",
                      "--elf-diff",
                      action="store_true",
                      default=False,
                      help="run elf_diff on output files")
    parser.add_option("",
                      "--master-branch",
                      type="string",
                      default="master",
                      help="master branch to use")
    parser.add_option("",
                      "--no-merge-base",
                      action="store_true",
                      default=False,
                      help="do not use the merge-base for testing, do a direct comparison between branches")
    parser.add_option("",
                      "--no-waf-consistent-builds",
                      action="store_true",
                      default=False,
                      help="do not use the --consistent-builds waf command-line option (for older branches)")
    parser.add_option("",
                      "--branch",
                      type="string",
                      default=None,
                      help="branch to compare")
    parser.add_option("",
                      "--vehicle",
                      action='append',
                      default=[],
                      help="vehicle to build for")
    parser.add_option("",
                      "--show-empty",
                      action='store_true',
                      default=False,
                      help="Show result lines even if no builds were done for the board")
    parser.add_option("",
                      "--board",
                      action='append',
                      default=[],
                      help="board to build for")
    parser.add_option("",
                      "--extra-hwdef",
                      default=[],
                      action="append",
                      help="configure with this extra hwdef file")
    parser.add_option("",
                      "--extra-hwdef-branch",
                      default=[],
                      action="append",
                      help="configure with this extra hwdef file only on new branch")
    parser.add_option("",
                      "--extra-hwdef-master",
                      default=[],
                      action="append",
                      help="configure with this extra hwdef file only on merge/master branch")
    parser.add_option("",
                      "--all-boards",
                      action='store_true',
                      default=False,
                      help="Build all boards")
    parser.add_option("",
                      "--all-vehicles",
                      action='store_true',
                      default=False,
                      help="Build all vehicles")
    parser.add_option("",
                      "--parallel-copies",
                      type=int,
                      default=None,
                      help="Copy source dir this many times, build from those copies in parallel")
    parser.add_option("-j",
                      "--jobs",
                      type=int,
                      default=None,
                      help="Passed to waf configure -j; number of build jobs.  If running with --parallel-copies, this is divided by the number of remaining threads before being passed.")  # noqa
    cmd_opts, cmd_args = parser.parse_args()

    vehicle = []
    for v in cmd_opts.vehicle:
        vehicle.extend(v.split(','))
    if len(vehicle) == 0:
        vehicle.append("plane")

    board = []
    for b in cmd_opts.board:
        board.extend(b.split(','))
    if len(board) == 0:
        board.append("MatekF405-Wing")

    x = SizeCompareBranches(
        branch=cmd_opts.branch,
        master_branch=cmd_opts.master_branch,
        board=board,
        vehicle=vehicle,
        extra_hwdef=cmd_opts.extra_hwdef,
        extra_hwdef_branch=cmd_opts.extra_hwdef_branch,
        extra_hwdef_master=cmd_opts.extra_hwdef_master,
        run_elf_diff=(cmd_opts.elf_diff),
        all_vehicles=cmd_opts.all_vehicles,
        all_boards=cmd_opts.all_boards,
        use_merge_base=not cmd_opts.no_merge_base,
        waf_consistent_builds=not cmd_opts.no_waf_consistent_builds,
        show_empty=cmd_opts.show_empty,
        parallel_copies=cmd_opts.parallel_copies,
        jobs=cmd_opts.jobs,
    )
    x.run()
