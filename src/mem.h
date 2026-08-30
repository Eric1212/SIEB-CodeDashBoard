#ifndef CDB_MEM_H
#define CDB_MEM_H

/* Rend au noyau les pages du tas déjà libérées. Le pourquoi — free() ne rend
 * rien, le trim oui, et son coût mesuré — est écrit dans mem.c. */
void cdb_mem_trim(void);

#endif
