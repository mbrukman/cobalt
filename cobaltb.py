#!/usr/bin/env python
# Copyright 2016 The Fuchsia Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The Cobalt build system command-line interface."""

import argparse
import logging
import os
import shutil
import subprocess
import sys

import tools.cpplint as cpplint
import tools.golint as golint
import tools.process_starter as process_starter
import tools.test_runner as test_runner

from tools.process_starter import DEFAULT_SHUFFLER_PORT
from tools.process_starter import DEFAULT_ANALYZER_SERVICE_PORT
from tools.process_starter import DEFAULT_REPORT_MASTER_PORT
from tools.process_starter import DEMO_CONFIG_DIR
from tools.process_starter import SHUFFLER_DEMO_CONFIG_FILE

THIS_DIR = os.path.abspath(os.path.dirname(__file__))
OUT_DIR = os.path.abspath(os.path.join(THIS_DIR, 'out'))
SYSROOT_DIR = os.path.abspath(os.path.join(THIS_DIR, 'sysroot'))

IMAGES = ["analyzer", "shuffler"]

GCE_PROJECT = "shuffler-test"
GCE_CLUSTER = "cluster-1"
GCE_TAG = "us.gcr.io/google.com/%s" % GCE_PROJECT

A_BT_INSTANCE = "cobalt-analyzer"
A_BT_TABLE = "observations"
A_BT_TABLE_NAME = "projects/google.com:%s/instances/%s/tables/%s" \
                % (GCE_PROJECT, A_BT_INSTANCE, A_BT_TABLE)

_logger = logging.getLogger()
_verbose_count = 0

def _initLogging(verbose_count):
  """Ensures that the logger (obtained via logging.getLogger(), as usual) is
  initialized, with the log level set as appropriate for |verbose_count|
  instances of --verbose on the command line."""
  assert(verbose_count >= 0)
  if verbose_count == 0:
    level = logging.WARNING
  elif verbose_count == 1:
    level = logging.INFO
  else:  # verbose_count >= 2
    level = logging.DEBUG
  logging.basicConfig(format="%(relativeCreated).3f:%(levelname)s:%(message)s")
  logger = logging.getLogger()
  logger.setLevel(level)
  logger.debug("Initialized logging: verbose_count=%d, level=%d" %
               (verbose_count, level))

def ensureDir(dir_path):
  """Ensures that the directory at |dir_path| exists. If not it is created.

  Args:
    dir_path{string} The path to a directory. If it does not exist it will be
    created.
  """
  if not os.path.exists(dir_path):
    os.makedirs(dir_path)

def setGCEImages(args):
  """Sets the list of GCE images to be built/deployed/started and stopped.

  Args:
    args{list} List of parsed command line arguments.
  """
  global IMAGES
  if args.shuffler_gce:
    IMAGES = ["shuffler"]
  elif args.analyzer_gce:
    IMAGES = ["analyzer"]

def _setup(args):
  subprocess.check_call(["git", "submodule", "init"])
  subprocess.check_call(["git", "submodule", "update"])
  subprocess.check_call(["./setup.sh", "-d"])

def _build(args):
  ensureDir(OUT_DIR)
  savedir = os.getcwd()
  os.chdir(OUT_DIR)
  subprocess.check_call(['cmake', '-G', 'Ninja','..'])
  subprocess.check_call(['ninja'])
  os.chdir(savedir)

def _lint(args):
  cpplint.main()
  golint.main()

# Specifiers of subsets of tests to run
TEST_FILTERS =['all', 'gtests', 'nogtests', 'gotests', 'nogotests',
               'btemulator', 'nobtemulator', 'e2e', 'noe2e', 'cloud_bt']

# Returns 0 if all tests return 0, otherwise returns 1.
def _test(args):
  # A map from positive filter specifiers to the list of test directories
  # it represents. Note that 'cloud_bt' tests are special. They are not
  # included in 'all'. They are only run if asked for explicitly.
  FILTER_MAP = {
    'all': ['gtests', 'go_tests', 'gtests_btemulator', 'e2e_tests'],
    'gtests': ['gtests'],
    'gotests' : ['go_tests'],
    'btemulator': ['gtests_btemulator'],
    'e2e': ['e2e_tests'],
    'cloud_bt' : ['gtests_cloud_bt']
  }

  # A list of test directories for which the contained tests assume the
  # existence of a running instance of the Bigtable Emulator.
  NEEDS_BT_EMULATOR=['gtests_btemulator', 'e2e_tests']

  # A list of test directories for which the contained tests assume the
  # existence of a running instance of the Cobalt processes (Shuffler,
  # Analyzer Service, Report Master.)
  NEEDS_COBALT_PROCESSES=['e2e_tests']

  # Get the list of test directories we should run.
  if args.tests.startswith('no'):
    test_dirs = [test_dir for test_dir in FILTER_MAP['all']
        if test_dir not in FILTER_MAP[args.tests[2:]]]
  else:
    test_dirs = FILTER_MAP[args.tests]

  success = True
  print ("Will run tests in the following directories: %s." %
      ", ".join(test_dirs))
  for test_dir in test_dirs:
    start_bt_emulator = (test_dir in NEEDS_BT_EMULATOR)
    start_cobalt_processes = (test_dir in NEEDS_COBALT_PROCESSES)
    test_args = None
    if (test_dir == 'gtests_cloud_bt'):
      if args.bigtable_instance_name == '':
        print '--bigtable_instance_name must be specified'
        success = False
        break
      test_args = [
          "--bigtable_project_name=%s" % args.bigtable_project_name,
          "--bigtable_instance_name=%s" % args.bigtable_instance_name
      ]
    if (test_dir == 'e2e_tests'):
      test_args = [
          "-analyzer_uri=localhost:%d" % DEFAULT_ANALYZER_SERVICE_PORT,
          "-shuffler_uri=localhost:%d" % DEFAULT_SHUFFLER_PORT,
          "-report_master_uri=localhost:%d" % DEFAULT_REPORT_MASTER_PORT,
          ("-observation_querier_path=%s" %
              process_starter.OBSERVATION_QUERIER_PATH),
          "-test_app_path=%s" % process_starter.TEST_APP_PATH,
          "-sub_process_v=%d"%_verbose_count
      ]
    print '********************************************************'
    success = (test_runner.run_all_tests(
        test_dir, start_bt_emulator=start_bt_emulator,
        start_cobalt_processes=start_cobalt_processes,
        verbose_count=_verbose_count,
        test_args=test_args) == 0) and success

  print
  if success:
    print '******************* ALL TESTS PASSED *******************'
    return 0
  else:
    print '******************* SOME TESTS FAILED *******************'
    return 1

# Files and directories in the out directory to NOT delete when doing
# a partial clean.
TO_SKIP_ON_PARTIAL_CLEAN = [
  'CMakeFiles', 'third_party', '.ninja_deps', '.ninja_log', 'CMakeCache.txt',
  'build.ninja', 'cmake_install.cmake', 'rules.ninja'
]

def _clean(args):
  if args.full:
    print "Deleting the out directory..."
    shutil.rmtree(OUT_DIR, ignore_errors=True)
  else:
    print "Doing a partial clean. Pass --full for a full clean."
    if not os.path.exists(OUT_DIR):
      return
    for f in os.listdir(OUT_DIR):
      if not f in TO_SKIP_ON_PARTIAL_CLEAN:
        full_path = os.path.join(OUT_DIR, f)
        if os.path.isfile(full_path):
          os.remove(full_path)
        else:
           shutil.rmtree(full_path, ignore_errors=True)

def _start_bigtable_emulator(args):
  process_starter.start_bigtable_emulator()

def _start_shuffler(args):
  process_starter.start_shuffler(port=args.port,
                                 analyzer_uri=args.analyzer_uri,
                                 use_memstore=args.use_memstore,
                                 erase_db=(not args.keep_existing_db),
                                 config_file=args.config_file,
                                 # Because it makes the demo more interesting
                                 # we use verbose_count at least 3.
                                 verbose_count=max(3, _verbose_count))

def _start_analyzer_service(args):
  process_starter.start_analyzer_service(port=args.port,
      # Because it makes the demo more interesting
      # we use verbose_count at least 3.
      verbose_count=max(3, _verbose_count))

def _start_report_master(args):
  process_starter.start_report_master(port=args.port,
                                      cobalt_config_dir=args.cobalt_config_dir,
                                      verbose_count=_verbose_count)

def _start_test_app(args):
  process_starter.start_test_app(shuffler_uri=args.shuffler_uri,
                                 analyzer_uri=args.analyzer_uri,
                                 verbose_count=_verbose_count)

def _start_report_client(args):
  process_starter.start_report_client(
      report_master_uri=args.report_master_uri,
      verbose_count=_verbose_count)

def _start_observation_querier(args):
  process_starter.start_observation_querier(verbose_count=_verbose_count)

def _gce_build(args):
  setGCEImages(args)

  # Copy over the dependencies for the cobalt base image
  cobalt = "%s/cobalt" % OUT_DIR

  if not os.path.exists(cobalt):
    os.mkdir(cobalt)

  for dep in ["lib/libprotobuf.so.10",
              "lib/libgoogleapis.so",
              "lib/libgrpc++.so.1",
              "lib/libgrpc.so.1",
              "lib/libunwind.so.1",
              "share/grpc/roots.pem",
             ]:
    shutil.copy("%s/%s" % (SYSROOT_DIR, dep), cobalt)

  # Copy configuration files
  for conf in ["registered_metrics.txt",
               "registered_encodings.txt",
               "registered_reports.txt"
              ]:
    shutil.copy("%s/config/demo/%s" % (THIS_DIR, conf),
                "%s/analyzer/" % OUT_DIR)

  # Build all images
  for i in ["cobalt"] + IMAGES:
    # copy over the dockerfile
    dstdir = "%s/%s" % (OUT_DIR, i)
    shutil.copy("%s/docker/%s/Dockerfile" % (THIS_DIR, i), dstdir)

    subprocess.check_call(["docker", "build", "-t", i, dstdir])

def _gce_push(args):
  setGCEImages(args)

  for i in IMAGES:
    tag = "%s/%s" % (GCE_TAG, i)
    subprocess.check_call(["docker", "tag", i, tag])
    subprocess.check_call(["gcloud", "docker", "--", "push", tag])

def kube_setup():
  subprocess.check_call(["gcloud", "container", "clusters", "get-credentials",
                         GCE_CLUSTER, "--project",
                         "google.com:%s" % GCE_PROJECT])

def _gce_start(args):
  setGCEImages(args)

  kube_setup()

  for i in IMAGES:
    print("Starting %s" % i)

    if (i == "analyzer"):
      args = ["-table", A_BT_TABLE_NAME,
              "-metrics", "/etc/cobalt/registered_metrics.txt",
              "-reports", "/etc/cobalt/registered_reports.txt",
              "-encodings", "/etc/cobalt/registered_encodings.txt"]

      subprocess.check_call(["kubectl", "run", i, "--image=%s/%s" % (GCE_TAG, i),
                             "--port=8080", "--"] + args)
    else:
      subprocess.check_call(["kubectl", "run", i, "--image=%s/%s" % (GCE_TAG, i),
                             "--port=50051"])

    subprocess.check_call(["kubectl", "expose", "deployment", i,
                           "--type=LoadBalancer"])

def _gce_stop(args):
  setGCEImages(args)

  kube_setup()

  for i in IMAGES:
    print("Stopping %s" % i)

    subprocess.check_call(["kubectl", "delete", "service,deployment", i,])

def main():
  parser = argparse.ArgumentParser(description='The Cobalt command-line '
      'interface.')

  # Note(rudominer) A note about the handling of optional arguments here.
  # We create |parent_parser| and make it a parent of all of our sub parsers.
  # When we want to add a global optional argument (i.e. one that applies
  # to all sub-commands such as --verbose) we add the optional argument
  # to both |parent_parser| and |parser|. The reason for this is that
  # that appears to be the only way to get the help string  to show up both
  # when the top-level command is invoked and when
  # a sub-command is invoked.
  #
  # In other words when the user types:
  #
  #                python cobaltb.py -h
  #
  # and also when the user types
  #
  #                python cobaltb.py test -h
  #
  # we want to show the help for the --verbose option.
  parent_parser = argparse.ArgumentParser(add_help=False)

  parser.add_argument('--verbose',
    help='Be verbose (multiple times for more)',
    default=0, dest='verbose_count', action='count')
  parent_parser.add_argument('--verbose',
    help='Be verbose (multiple times for more)',
    default=0, dest='verbose_count', action='count')

  subparsers = parser.add_subparsers()

  sub_parser = subparsers.add_parser('setup', parents=[parent_parser],
    help='Sets up the build environment.')
  sub_parser.set_defaults(func=_setup)

  sub_parser = subparsers.add_parser('build', parents=[parent_parser],
    help='Builds Cobalt.')
  sub_parser.set_defaults(func=_build)

  sub_parser = subparsers.add_parser('lint', parents=[parent_parser],
    help='Run language linters on all source files.')
  sub_parser.set_defaults(func=_lint)

  sub_parser = subparsers.add_parser('test', parents=[parent_parser],
    help='Runs Cobalt tests. You must build first.')
  sub_parser.set_defaults(func=_test)
  sub_parser.add_argument('--tests', choices=TEST_FILTERS,
      help='Specify a subset of tests to run. Default=all',
      default='all')
  sub_parser.add_argument('--bigtable_project_name',
      help='Specify a Cloud project against which to run the cloud_bt tests.'
      ' Optional. Default=google.com:shuffler-test.'
      ' Only used when --tests=cloud_bt', default='google.com:shuffler-test')
  sub_parser.add_argument('--bigtable_instance_name',
      help='Specify a Cloud Bigtable instance within the specified Cloud'
      ' project against which to run the cloud_bt tests.'
      ' Used and required only when --tests=cloud_bt.', default='')

  sub_parser = subparsers.add_parser('clean', parents=[parent_parser],
    help='Deletes some or all of the build products.')
  sub_parser.set_defaults(func=_clean)
  sub_parser.add_argument('--full',
      help='Delete the entire "out" directory.',
      action='store_true')

  start_parser = subparsers.add_parser('start',
    help='Start one of the Cobalt processes running locally.')
  start_subparsers = start_parser.add_subparsers()

  sub_parser = start_subparsers.add_parser('shuffler',
      parents=[parent_parser], help='Start the Shuffler running locally.')
  sub_parser.set_defaults(func=_start_shuffler)
  sub_parser.add_argument('--port',
      help='The port on which the Shuffler should listen. '
           'Default=%s.' % DEFAULT_SHUFFLER_PORT,
      default=DEFAULT_SHUFFLER_PORT)
  sub_parser.add_argument('--analyzer_uri',
      help='Default=localhost:%s'%DEFAULT_ANALYZER_SERVICE_PORT,
      default='localhost:%s'%DEFAULT_ANALYZER_SERVICE_PORT)
  sub_parser.add_argument('-use_memstore',
      help='Default: False, use persistent LevelDB Store.',
      action='store_true')
  sub_parser.add_argument('-keep_existing_db',
      help='When using LevelDB should any previously persisted data be kept? '
      'Default=False, erase the DB before starting the Shuffler.',
      action='store_true')
  sub_parser.add_argument('--config_file',
      help='Path to the Shuffler configuration file. '
           'Default=%s' % SHUFFLER_DEMO_CONFIG_FILE,
      default=SHUFFLER_DEMO_CONFIG_FILE)

  sub_parser = start_subparsers.add_parser('analyzer_service',
      parents=[parent_parser], help='Start the Analyzer Service running locally'
          ' and connected to a local instance of the Bigtable Emulator.')
  sub_parser.set_defaults(func=_start_analyzer_service)
  sub_parser.add_argument('--port',
      help='The port on which the Analyzer service should listen. '
           'Default=%s.' % DEFAULT_ANALYZER_SERVICE_PORT,
      default=DEFAULT_ANALYZER_SERVICE_PORT)

  sub_parser = start_subparsers.add_parser('report_master',
      parents=[parent_parser], help='Start the Analyzer ReportMaster Service '
          'running locally and connected to a local instance of the Bigtable'
          'Emulator.')
  sub_parser.set_defaults(func=_start_report_master)
  sub_parser.add_argument('--port',
      help='The port on which the ReportMaster should listen. '
           'Default=%s.' % DEFAULT_REPORT_MASTER_PORT,
      default=DEFAULT_REPORT_MASTER_PORT)
  sub_parser.add_argument('--cobalt_config_dir',
      help='Path of directory containing Cobalt configuration files. '
           'Default=%s' % DEMO_CONFIG_DIR,
      default=DEMO_CONFIG_DIR)

  sub_parser = start_subparsers.add_parser('test_app',
      parents=[parent_parser], help='Start the Cobalt test client app.')
  sub_parser.set_defaults(func=_start_test_app)
  sub_parser.add_argument('--shuffler_uri',
      help='Default=localhost:%s' % DEFAULT_SHUFFLER_PORT,
      default='localhost:%s' % DEFAULT_SHUFFLER_PORT)
  sub_parser.add_argument('--analyzer_uri',
      help='Default=localhost:%s'%DEFAULT_ANALYZER_SERVICE_PORT,
      default='localhost:%s'%DEFAULT_ANALYZER_SERVICE_PORT)

  sub_parser = start_subparsers.add_parser('report_client',
      parents=[parent_parser], help='Start the Cobalt report client.')
  sub_parser.set_defaults(func=_start_report_client)
  sub_parser.add_argument('--report_master_uri',
      help='Default=localhost:%s' % DEFAULT_REPORT_MASTER_PORT,
      default='localhost:%s' % DEFAULT_REPORT_MASTER_PORT)

  sub_parser = start_subparsers.add_parser('observation_querier',
      parents=[parent_parser], help='Start the Cobalt ObservationStore '
                                    'querying tool.')
  sub_parser.set_defaults(func=_start_observation_querier)

  sub_parser = start_subparsers.add_parser('bigtable_emulator',
    parents=[parent_parser],
    help='Start the Bigtable Emulator running locally.')
  sub_parser.set_defaults(func=_start_bigtable_emulator)

  sub_parser = subparsers.add_parser('gce_build', parents=[parent_parser],
    help='Builds Docker images for GCE.')
  sub_parser.set_defaults(func=_gce_build)
  sub_parser.add_argument('--a',
      help='Builds Analyzer Docker image for GCE.',
      action='store_true', dest='analyzer_gce')
  sub_parser.add_argument('--s',
      help='Builds Shuffler Docker image for GCE.',
      action='store_true', dest='shuffler_gce')

  sub_parser = subparsers.add_parser('gce_push', parents=[parent_parser],
    help='Push docker images to GCE.')
  sub_parser.set_defaults(func=_gce_push)
  sub_parser.add_argument('--a',
      help='Push Analyzer Docker image to GCE.',
      action='store_true', dest='analyzer_gce')
  sub_parser.add_argument('--s',
      help='Push Shuffler Docker image to GCE.',
      action='store_true', dest='shuffler_gce')

  sub_parser = subparsers.add_parser('gce_start', parents=[parent_parser],
    help='Start GCE instances.')
  sub_parser.set_defaults(func=_gce_start)
  sub_parser.add_argument('--a',
      help='Starts Analyzer GCE instance.',
      action='store_true', dest='analyzer_gce')
  sub_parser.add_argument('--s',
      help='Starts Shuffler GCE instance.',
      action='store_true', dest='shuffler_gce')

  sub_parser = subparsers.add_parser('gce_stop', parents=[parent_parser],
    help='Stop GCE instances.')
  sub_parser.set_defaults(func=_gce_stop)
  sub_parser.add_argument('--a',
      help='Stops Analyzer GCE instance.',
      action='store_true', dest='analyzer_gce')
  sub_parser.add_argument('--s',
      help='Stops Shuffler GCE instance.',
      action='store_true', dest='shuffler_gce')

  args = parser.parse_args()
  global _verbose_count
  _verbose_count = args.verbose_count
  _initLogging(_verbose_count)

  # Extend paths to include third-party dependencies
  os.environ["PATH"] = \
      "%s/bin" % SYSROOT_DIR \
      + os.pathsep + "%s/gcloud/google-cloud-sdk/bin" % SYSROOT_DIR \
      + os.pathsep + os.environ["PATH"]
  os.environ["LD_LIBRARY_PATH"] = "%s/lib" % SYSROOT_DIR

  os.environ["GOOGLE_APPLICATION_CREDENTIALS"] = os.path.abspath(os.path.join(
      THIS_DIR, 'service_account_credentials.json'))

  os.environ["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = os.path.abspath(
      os.path.join(SYSROOT_DIR, 'share', 'grpc', 'roots.pem'))

  os.environ["GOROOT"] = "%s/golang" % SYSROOT_DIR

  return args.func(args)


if __name__ == '__main__':
  sys.exit(main())
