#pragma once

#define STATUS_STRING  "\"ftpd v2.2\""

/*! Loop status */
typedef enum
{
  LOOP_CONTINUE, /*!< Continue looping */
  LOOP_RESTART,  /*!< Reinitialize */
  LOOP_EXIT,     /*!< Terminate looping */
} loop_status_t;

int           ftp_init(void);
loop_status_t ftp_loop(void);
void          ftp_exit(void);
