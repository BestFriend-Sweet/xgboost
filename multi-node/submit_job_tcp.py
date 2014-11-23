#!/usr/bin/python
"""
This is an example script to create a customized job submit
script using xgboost sync_tcp mode
"""
import sys
import os
import subprocess
# import the tcp_master.py
# add path to sync
sys.path.append(os.path.dirname(__file__)+'/../src/sync/')
import tcp_master as master

def mpi_submit(nslave, args):
    """
      customized submit script, that submit nslave jobs, each must contain args as parameter
      note this can be a lambda function containing additional parameters in input
      Parameters
         nslave number of slave process to start up
         args arguments to launch each job
              this usually includes the parameters of master_uri and parameters passed into submit
    """
    cmd = ' '.join(['mpirun -n %d' % nslave] + args)
    print cmd
    subprocess.check_call(cmd, shell = True)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print 'Usage: <nslave> <cmd>'
        exit(0)        
    # call submit, with nslave, the commands to run each job and submit function
    master.submit(int(sys.argv[1]), sys.argv[2:], fun_submit= mpi_submit)
