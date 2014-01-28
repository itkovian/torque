#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <signal.h>
#include <ctype.h>
#include <pthread.h>
#include "libpbs.h"
#include "server_limits.h"
#include "list_link.h"
#include "work_task.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "pbs_job.h"
#include "queue.h"
#include "pbs_error.h"
#include "acct.h"
#include "log.h"
#include "../lib/Liblog/pbs_log.h"
#include "../lib/Liblog/log_event.h"
#include "svrfunc.h"
#include "job_func.h"
#include "mutex_mgr.hpp"
#include "array.h"

#include "ji_mutex.h"
#include "mutex_mgr.hpp"

extern int LOGLEVEL;
extern int  svr_authorize_req(struct batch_request *preq, char *owner, char *submit_host);

extern struct work_task *apply_job_delete_nanny(struct job *, int);
extern int has_job_delete_nanny(struct job *);
extern void remove_stagein(job **pjob);
extern void change_restart_comment_if_needed(struct job *);
int issue_signal(job **, const char *, void(*)(batch_request *), void *, char *);

extern char *msg_unkarrayid;
extern char *msg_permlog;

void post_delete(struct work_task *pwt);

void array_delete_wt(struct work_task *ptask);
void          on_job_exit_task(struct work_task *);
int   delete_inactive_job(job **pjob_ptr, const char *Msg);

extern int LOGLEVEL;


/**
 * attempt_delete()
 * deletes a job differently depending on the job's state
 *
 * @return TRUE if the job was deleted, FALSE if skipped
 * @param pjob - a pointer to the job being handled
 * this functions starts with pjob->ji_mutex locked and
 * exit with pjob->ji_mutex unlocked.
 */
int attempt_delete(

  void *j) /* I */

  {
  int        skipped = FALSE;

  job       *pjob;
  time_t     time_now = time(NULL);

  /* job considered deleted if null */
  if (j == NULL)
    return(TRUE);

  pjob = (job *)j;

  mutex_mgr pjob_mutex(pjob->ji_mutex, true);

  if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_CHECKPOINT_FILE) != 0)
    {
    /* job has restart file at mom, change restart comment if failed */    
    change_restart_comment_if_needed(pjob);
    }

  if (pjob->ji_qs.ji_state == JOB_STATE_TRANSIT)
    {
    /* I'm not sure if this is still possible since the thread
     * waits on the job to finish transmiting, but I'll leave
     * this part here --dbeer */
    skipped = TRUE;
    
    return(!skipped);
    }  /* END if (pjob->ji_qs.ji_state == JOB_SUBSTATE_TRANSIT) */

  else if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_PRERUN)
    {
    /* we'll wait for the mom to get this job, then delete it */
    skipped = TRUE;
    }  /* END if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_PRERUN) */

  else if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING)
    {
    /* set up nanny */
    
    if (pjob->ji_has_delete_nanny == FALSE)
      {
      apply_job_delete_nanny(pjob, time_now + 60);
      
      /* need to issue a signal to the mom, but we don't want to sent an ack to the
       * client when the mom replies */
      issue_signal(&pjob, "SIGTERM", free_br, NULL, NULL);
      }

    if (pjob != NULL)
      {
      if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_CHECKPOINT_FILE) != 0)
        {
        /* job has restart file at mom, change restart comment if failed */
        change_restart_comment_if_needed(pjob);
        }
      }
    else
      pjob_mutex.set_unlock_on_exit(false);
    
    return(!skipped);
    }  /* END if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING) */

  delete_inactive_job(&pjob, NULL);
  
  if (pjob == NULL)
    pjob_mutex.set_unlock_on_exit(false);

  return(!skipped);
  } /* END attempt_delete() */







int req_deletearray(
    
  struct batch_request *preq)

  {
  job_array        *pa;

  char             *range;

  struct work_task *ptask;
  char              log_buf[LOCAL_LOG_BUF_SIZE];

  int               num_skipped = 0;
  char              owner[PBS_MAXUSER + 1];
  time_t            time_now = time(NULL);

  pa = get_array(preq->rq_ind.rq_delete.rq_objname);

  if (pa == NULL)
    {
    reply_ack(preq);
    return(PBSE_NONE);
    }

  mutex_mgr pa_mutex = mutex_mgr(pa->ai_mutex, true);
  /* check authorization */
  get_jobowner(pa->ai_qs.owner, owner);

  if (svr_authorize_req(preq, owner, pa->ai_qs.submit_host) == -1)
    {
    sprintf(log_buf, msg_permlog,
      preq->rq_type,
      "Array",
      preq->rq_ind.rq_delete.rq_objname,
      preq->rq_user,
      preq->rq_host);

    log_event(PBSEVENT_SECURITY,PBS_EVENTCLASS_JOB,preq->rq_ind.rq_delete.rq_objname,log_buf);

    req_reject(PBSE_PERM, 0, preq, NULL, "operation not permitted");
    return(PBSE_NONE);
    }

  /* get the range of jobs to iterate over */
  range = preq->rq_extend;
  if ((range != NULL) &&
      (strstr(range,ARRAY_RANGE) != NULL))
    {
    if (LOGLEVEL >= 5)
      {
      sprintf(log_buf, "delete array requested by %s@%s for %s (%s)",
            preq->rq_user,
            preq->rq_host,
            preq->rq_ind.rq_delete.rq_objname,
            range);

      log_record(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
      }

    /* parse the array range */
    num_skipped = delete_array_range(pa,range);

    if (num_skipped < 0)
      {
      /* ERROR */
      req_reject(PBSE_IVALREQ,0,preq,NULL,"Error in specified array range");
      return(PBSE_NONE);
      }
    }
  else
    {
    if (LOGLEVEL >= 5)
      {
      sprintf(log_buf, "delete array requested by %s@%s for %s",
        preq->rq_user,
        preq->rq_host,
        preq->rq_ind.rq_delete.rq_objname);

      log_record(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
      }

    if ((num_skipped = delete_whole_array(pa)) == NO_JOBS_IN_ARRAY)
      array_delete(pa);
    }

  if (num_skipped != NO_JOBS_IN_ARRAY)
    {
    pa_mutex.unlock();
    
    /* check if the array is gone */
    if (get_array(preq->rq_ind.rq_delete.rq_objname) != NULL)
      {
      /* if get_array() returns non null this is the same mutex we had before */
      /* ai_mutex comes out of get_array locked so we need to
         set the locked state to true */
      pa_mutex.set_lock_state(true);
      
      /* some jobs were not deleted.  They must have been running or had
         JOB_SUBSTATE_TRANSIT */
      if (num_skipped != 0)
        {
        ptask = set_task(WORK_Timed, time_now + 10, array_delete_wt, preq, FALSE);
        
        if (ptask)
          {
          return(PBSE_NONE);
          }
        }
      }
    }
  
  /* now that the whole array is deleted, we should mail the user if necessary */
  reply_ack(preq);

  return(PBSE_NONE);
  } /* END req_deletearray() */



/* if jobs were in the prerun state , this attempts to keep track
   of if it was called continuously on the same array for over 10 seconds.
   If that is the case then it deletes prerun jobs no matter what.
   if it has been less than 10 seconds or if there are jobs in
   other statest then req_deletearray is called again */

void array_delete_wt(
    
  struct work_task *ptask)

  {
  struct batch_request *preq;
  job_array            *pa;

  int                   i;

  char                  log_buf[LOCAL_LOG_BUF_SIZE];
  int                   num_jobs = 0;
  int                   num_prerun = 0;
  job                  *pjob;

  preq = get_remove_batch_request((char *)ptask->wt_parm1);
  
  free(ptask->wt_mutex);
  free(ptask);

  if (preq == NULL)
    return;

  pa = get_array(preq->rq_ind.rq_delete.rq_objname);

  if (pa == NULL)
    {
    /* jobs must have exited already */
    reply_ack(preq);

    return;
    }

  mutex_mgr array_mutex(pa->ai_mutex, true);

  for (i = 0; i < pa->ai_qs.array_size; i++)
    {
    if (pa->job_ids[i] == NULL)
      continue;
    
    if ((pjob = svr_find_job(pa->job_ids[i], FALSE)) == NULL)
      {
      free(pa->job_ids[i]);
      pa->job_ids[i] = NULL;
      }
    else
      {
      mutex_mgr job_mutex(pjob->ji_mutex, true);
      num_jobs++;
      
      if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_PRERUN)
        {
        num_prerun++;
        /* mom still hasn't gotten job?? delete anyway */
        
        if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_CHECKPOINT_FILE) != 0)
          {
          /* job has restart file at mom, do end job processing */
          change_restart_comment_if_needed(pjob);
          
          svr_setjobstate(pjob, JOB_STATE_EXITING, JOB_SUBSTATE_EXITING, FALSE);
          
          pjob->ji_momhandle = -1;
          
          /* force new connection */
          if (LOGLEVEL >= 7)
            {
            sprintf(log_buf, "calling on_job_exit from %s", __func__);
            log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, pjob->ji_qs.ji_jobid, log_buf);
            }
          
          set_task(WORK_Immed, 0, on_job_exit_task, strdup(pjob->ji_qs.ji_jobid), FALSE);
          }
        }
      else if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_StagedIn) != 0)
        {
        /* job has staged-in file, should remove them */
        remove_stagein(&pjob);
        
        if (pjob != NULL)
          {
          /* job_abt() calls svr_job_purge which will try to lock the array again */
          array_mutex.unlock();
          job_abt(&pjob, NULL);
          job_mutex.set_unlock_on_exit(false);
          pa = get_array(preq->rq_ind.rq_delete.rq_objname);
          if (pa != NULL)
            array_mutex.mark_as_locked();
          }
        else
          job_mutex.set_unlock_on_exit(false);
        }
      else
        {
        /* job_abt() calls svr_job_purge which will try to lock the array again */
        array_mutex.unlock();
        job_mutex.set_unlock_on_exit(false);

        job_abt(&pjob, NULL);
        pa = get_array(preq->rq_ind.rq_delete.rq_objname);
        if (pa != NULL)
          array_mutex.mark_as_locked();
        }
      } /* END if (ji_substate == JOB_SUBSTATE_PRERUN) */
    } /* END for each job in array */
  
  if (num_jobs == num_prerun)
    {
    reply_ack(preq);
    }
  else
    {
    req_deletearray(preq);
    }

  } /* END array_delete_wt() */


