/**
   @file transaction.c Defines functions for transaction execution
*/
/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 *
 * transaction.c is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * transaction.c is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "transaction.h"

#define MAX_ACTIVE_TRANSACTIONS_COUNT 10

AK_transaction_list LockTable[NUMBER_OF_KEYS];

extern pthread_mutex_t accessLockMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t newTransactionLockMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t endTransationTestLockMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t acquireLockMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t getMemoryBlocksMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t cond_lock  = PTHREAD_COND_INITIALIZER;
AK_observable_transaction *observable_transaction;
pthread_t activeThreads[MAX_ACTIVE_TRANSACTIONS_COUNT];

int activeTransactionsCount = 0;
int transactionsCount = 0;

/**
 * @author Frane Jakelić
 * @brief Calculates hash value for a given memory address. Hash values are used to identify location of locked resources.
 * @todo The current implementation is very limited it doesn't cope well with collision. recommendation use some better version of hash calculation. Maybe Knuth's memory address hashing function.
 * @param blockMemoryAddress integer representation of memory address, the hash value is calculated from this parameter.
 * @return integer containing the hash value of the passed memory address
 */
int AK_memory_block_hash(int blockMemoryAddress) {
    return blockMemoryAddress % NUMBER_OF_KEYS;
}

/**
 * @author Frane Jakelić
 * @brief Searches for a existing entry in hash list of active blocks
 * @param blockAddress integer representation of memory address.
 * @return pointer to the existing hash list entry
 */
AK_transaction_elem_P AK_search_existing_link_for_hook(int blockAddress){
	int hash = AK_memory_block_hash(blockAddress);
	AK_transaction_elem_P tmp = LockTable[hash].DLLHead;

	if(!tmp)return NULL;

	do{
		if(tmp->address == blockAddress) return tmp;
		tmp = tmp->nextBucket;
	} while(tmp != LockTable[hash].DLLHead);
	return NULL;
}

/**
 * @author Frane Jakelić
 * @brief Searches for a empty link for new active block, helper method in case of address collision
 * @param blockAddress integer representation of memory address.
 * @return pointer to empty location to store new active address
 */
AK_transaction_elem_P AK_search_empty_link_for_hook(int blockAddress){
	int hash = AK_memory_block_hash(blockAddress);

	if(!LockTable[hash].DLLHead){

		LockTable[hash].DLLHead = (AK_transaction_elem_P) malloc(sizeof (AK_transaction_elem));
		memset(LockTable[hash].DLLHead, 0, sizeof (AK_transaction_elem));

		return LockTable[hash].DLLHead;
	}else{
		return LockTable[hash].DLLHead;
	}

}

/**
 * @author Frane Jakelić
 * @brief Adds an element to the doubly linked list.
 * @param blockAddress integer representation of memory address.
 * @param type of lock issued to the provided memory address.
 * @return pointer to the newly created doubly linked element.
 */
AK_transaction_elem_P AK_add_hash_entry_list(int blockAddress, int type) {


    AK_transaction_elem_P root = AK_search_existing_link_for_hook(blockAddress);
    AK_transaction_elem_P bucket;
    if(root) return root;

    root = AK_search_empty_link_for_hook(blockAddress);
    if(!root->nextBucket){
    	bucket = root;
    	root->nextBucket = root;
    	root->prevBucket = root;
    }else{

    	bucket = (AK_transaction_elem_P) malloc(sizeof (AK_transaction_elem));
        memset(bucket, 0, sizeof (AK_transaction_elem));
        bucket->nextBucket = root;
        bucket->prevBucket = root->prevBucket;

        (*root->prevBucket).nextBucket = bucket;
        root->prevBucket = bucket;
    }

    bucket->transactionId = pthread_self();
    bucket->address = blockAddress;
    bucket->lock_type = type;
    bucket->observer_lock = AK_init_observer_lock();

    // NOTE: Uncomment this if we need to register observer lock to observable_transaction
    //AK_transaction_register_observer(observable_transaction, bucket->observer_lock->observer);

    return bucket;
}

/**
 * @author Frane Jakelić
 * @brief Deletes a specific element in the lockTable doubly linked list.
 * @param blockAddress integer representation of memory address.
 * @return integer OK or NOT_OK based on success of finding the specific element in the list.
 */
int AK_delete_hash_entry_list(int blockAddress) {
    int hash = AK_memory_block_hash(blockAddress);
    AK_transaction_elem_P elemDelete = AK_search_existing_link_for_hook(blockAddress);

    if (elemDelete) {

        (*elemDelete->prevBucket).nextBucket = elemDelete->nextBucket;
        (*elemDelete->nextBucket).prevBucket = elemDelete->prevBucket;


        if (elemDelete == LockTable[hash].DLLHead && elemDelete->nextBucket != elemDelete) {
            LockTable[hash].DLLHead = elemDelete->nextBucket;
        } else if(elemDelete->nextBucket == elemDelete){
            LockTable[hash].DLLHead = NULL;
        }

        elemDelete->prevBucket = NULL;
        elemDelete->nextBucket = NULL;

        return OK;

    } else {
        return NOT_OK;
    }
}

/**
 * @author Frane Jakelić
 * @brief Searches for a specific entry in the Locks doubly linked list using the transaction id as it's key.
 * @param memoryAddress integer representation of memory address.
 * @param id integer representation of transaction id.
 * @return NULL pointer if the element is not found otherwise it returns a pointer to the found element
 */
AK_transaction_lock_elem_P AK_search_lock_entry_list_by_key(AK_transaction_elem_P Lockslist, int memoryAddress, pthread_t id) {

    AK_transaction_lock_elem_P tmpElem = Lockslist->DLLLocksHead;

    if (!tmpElem || pthread_equal(tmpElem->TransactionId,id)) {
        return tmpElem;
    }
    
    int counter = 0;
    while (counter++ <= NUMBER_OF_KEYS) {
        if (pthread_equal(tmpElem->TransactionId,id)) {
            return tmpElem;
        }
        tmpElem = tmpElem->nextLock;
    }
    return NULL;
}

/**
 * @author Frane Jakelić
 * @brief Deletes a specific entry in the Locks doubly linked list using the transaction id as it's key.
 * @param blockAddress integer representation of memory address.
 * @param id integer representation of transaction id.
 * @return int OK or NOT_OK based on success of finding the specific element in the list.
 */
int AK_delete_lock_entry_list(int blockAddress, pthread_t id) {
	AK_transaction_elem_P elemListHolder = AK_search_existing_link_for_hook(blockAddress);
    AK_transaction_lock_elem_P elemDelete = AK_search_lock_entry_list_by_key(elemListHolder,blockAddress, id);

    if (!elemDelete) { return NOT_OK;}
    
    do {
        /* printf("##########################\n# Lock Released		 #\n#------------------------#\n# Lock	ID:%lu		 #\n#------------------------#\n# LockedAddress:%i	 #\n##########################\n\n", (unsigned long) elemDelete->TransactionId, elemListHolder->address); */

        (*elemDelete->prevLock).nextLock = elemDelete->nextLock;
        (*elemDelete->nextLock).prevLock = elemDelete->prevLock;

        if (elemDelete == elemListHolder->DLLLocksHead && elemDelete->nextLock != elemDelete) {

            elemListHolder->DLLLocksHead = elemDelete->nextLock;

        } else if (elemDelete == elemListHolder->DLLLocksHead) {

            elemListHolder->DLLLocksHead = NULL;
        }

        elemDelete->prevLock = NULL;
        elemDelete->nextLock = NULL;

        elemDelete = AK_search_lock_entry_list_by_key(elemListHolder,blockAddress, id);

    } while (elemDelete);

    return OK;


}

/**
 * @author Frane Jakelić
 * @brief Based on the parameters puts an transaction action in waiting phase or let's the transaction do it's actions.
 * @param lockHolder pointer to the hash list entry that is entitled to the specific memory address.
 * @param type of lock issued to the provided memory address.
 * @param transactionId integer representation of transaction id.
 * @param lock pointer to the lock element that is being tested.
 * @return int PASS_LOCK_QUEUE or WAIT_FOR_UNLOCK based on the rules described inside the function.
 */
int AK_isLock_waiting(AK_transaction_elem_P lockHolder, int type, pthread_t transactionId, AK_transaction_lock_elem_P lock) {
    AK_transaction_lock_elem_P tmp = lockHolder->DLLLocksHead;

    
	if (tmp == lock) {
		lockHolder->lock_type = type;
		return PASS_LOCK_QUEUE;
	} else if (tmp->lock_type == SHARED_LOCK) {
        //BEGIN SHARED LOCK
		if (type == SHARED_LOCK) {
			return PASS_LOCK_QUEUE;

		} else if (type == EXCLUSIVE_LOCK) {
            return WAIT_FOR_UNLOCK;
		}
        //END SHARED LOCK

	} else if (tmp->lock_type == EXCLUSIVE_LOCK) {
        if ( pthread_equal(tmp->TransactionId, transactionId) ) {
            return PASS_LOCK_QUEUE;
        }
	}

	return WAIT_FOR_UNLOCK;
}



/**
 * @author Frane Jakelić
 * @brief Adds an element to the locks doubly linked list.
 * @param memoryAddress integer representation of memory address.
 * @param type of lock issued to the provided memory address.
 * @param transactionId integer representation of transaction id.
 * @return pointer to the newly created Locks doubly linked element.
 */
AK_transaction_lock_elem_P AK_add_lock(AK_transaction_elem_P HashList, int type, pthread_t transactionId) {

    AK_transaction_lock_elem_P root = HashList->DLLLocksHead;
    AK_transaction_lock_elem_P lock = (AK_transaction_lock_elem_P) malloc(sizeof (AK_transaction_lock_elem));
    memset(lock, 0, sizeof (AK_transaction_lock_elem));

    if (!root) {
    	HashList->DLLLocksHead = lock;
        lock->prevLock = lock;
        lock->nextLock = lock;

    } else {
        lock->nextLock = root;
        lock->prevLock = root->prevLock;
        (*root->prevLock).nextLock = lock;
        root->prevLock = lock;
    }

    lock->TransactionId = transactionId;
    lock->lock_type = type;
    
    lock->isWaiting = AK_isLock_waiting(HashList, type, transactionId, lock);
    return lock;
}

/**
 * @author Frane Jakelić
 * @brief Helper function that determines if there is a hash LockTable entry that corresponds to the given memory address. And if there isn't an entry the function calls for the creation of the Locks list holder.
 * @param memoryAddress integer representation of memory address.
 * @param type of lock issued to the provided memory address.
 * @param transactionId integer representation of transaction id.
 * @return pointer to the newly created Locks doubly linked element.
 */
AK_transaction_lock_elem_P AK_create_lock(int blockAddress, int type, pthread_t transactionId) {
    AK_transaction_elem_P elem = AK_search_existing_link_for_hook(blockAddress);

    if (!elem) {
        elem = AK_add_hash_entry_list(blockAddress, type);
    }
    return AK_add_lock(elem, type, transactionId);
}



/**
 * @author Frane Jakelić updated by Ivan Pusic
 * @brief Main interface function for the transaction API. It is responsible for the whole process of creating a new lock.
 * @todo Implement a better deadlock detection. This method uses a very simple approach. It waits for 60sec before it restarts a transaction.
 * @param memoryAddress integer representation of memory address.
 * @param type of lock issued to the provided memory address.
 * @param transactionId integer representation of transaction id.
 * @return OK or NOT_OK based on the success of the function.
 */

int AK_acquire_lock(int memoryAddress, int type, pthread_t transactionId) {
    pthread_mutex_lock(&acquireLockMutex);
    AK_transaction_lock_elem_P lock = AK_create_lock(memoryAddress, type, transactionId);
    pthread_mutex_unlock(&acquireLockMutex);
    int counter = 0;
    if(!lock->isWaiting){
    	//TODO Add deadlock test, partial implementation of tarjan test available in auxiliary.c
    }
    AK_transaction_lock_elem_P tmp = AK_search_existing_link_for_hook(memoryAddress);

    while (!lock->isWaiting) {
        /* printf("################\n# Lock Waiting		 #\n#------------------------#\n# Lock	ID:%lu	TYPE:%i	 #\n#------------------------#\n# LockedAddress:%i	 #\n##########################\n\n",(unsigned long) lock->TransactionId, lock->lock_type, memoryAddress); */
        pthread_mutex_lock(&acquireLockMutex);
        pthread_cond_wait(&cond_lock, &acquireLockMutex);
        pthread_mutex_unlock(&acquireLockMutex);
        
        lock->isWaiting = AK_isLock_waiting(tmp, type, transactionId, lock);
    }
    
    if (counter > 0) {

    	/* printf("################\n# Lock Granted after wait#\n#------------------------#\n# Lock	ID:%lu	TYPE:%i	 #\n#------------------------#\n# LockedAddress:%i	 #\n##########################\n\n", (unsigned long)lock->TransactionId, lock->lock_type, memoryAddress); */

    } else {

    	/* printf("##########################\n# Lock Granted		 #\n#------------------------#\n# Lock	ID:%lu	TYPE:%i	 #\n#------------------------#\n# LockedAddress:%i	 #\n##########################\n\n", (unsigned long)lock->TransactionId, lock->lock_type, memoryAddress); */
    }

    return OK;
}

/**
 * @author Frane Jakelić updated by Ivan Pusic
 * @brief Main interface function for the transaction API. It is responsible for the whole process releasing locks acquired by a transaction. The locks are released either by COMMIT or ABORT .
 * @param adresses linked list of memory addresses locked by the transaction.
 * @param transactionId integer representation of transaction id.
 */
void AK_release_locks(AK_memoryAddresses_link addressesTmp, pthread_t transactionId) {
    pthread_mutex_lock(&accessLockMutex);

    while (addressesTmp->nextElement != NULL) {

        // NOTE: Uncomment this if we need to unregister transaction lock from observable_transaction
        // Unregister observer and deallocate used memory (dealocating memory is in observable.c unregister_observer)
        /* AK_observer *observer = AK_search_existing_link_for_hook(addressesTmp->adresa)->observer_lock->observer; */
        /* if(observer != NULL) { */
        /*     AK_transaction_unregister_observer(observable_transaction, observer); */
        /*     observer = NULL; */
        /* } */
         
        AK_delete_lock_entry_list(addressesTmp->adresa, transactionId);

        AK_transaction_lock_elem_P tmp = AK_search_existing_link_for_hook(addressesTmp->adresa)->DLLLocksHead;

        AK_transaction_lock_elem_P anchor = tmp;

        if (tmp != NULL && tmp->nextLock != anchor && !pthread_equal(tmp->nextLock->TransactionId,transactionId)) {
            while (tmp->nextLock->isWaiting == WAIT_FOR_UNLOCK && tmp->nextLock != anchor) {
                
                //tmp->nextLock->isWaiting = AK_isLock_waiting(anchor,
                //                                             tmp->nextLock->lock_type, tmp->nextLock->TransactionId,
                //                                             tmp->nextLock);
                tmp = tmp->nextLock;
            };
        }
        // notify observable transaction about lock release
        observable_transaction->AK_lock_released();
        addressesTmp = addressesTmp->nextElement;
    }
    pthread_mutex_unlock(&accessLockMutex);
}

/**
 * @author Frane Jakelić
 * @brief Method that appends all addresses affected by the transaction
 * @param addressList pointer to the linked list where the addresses are stored.
 * @param tblName table name used in the transaction
 * @return OK or NOT_OK based on the success of the function.
 */
int AK_get_memory_blocks(char *tblName, AK_memoryAddresses_link addressList) {
    table_addresses *addresses = (table_addresses*) AK_get_table_addresses(tblName);
    if (addresses->address_from[0] == 0)
        return NOT_OK;

    if (addressList->nextElement != NULL) {
        addressList->nextElement = NULL;
    }

    int i = 0, j;
    AK_memoryAddresses_link tmp = addressList;

    for (j = addresses->address_from[ i ]; j < addresses->address_to[ i ]; j++) {
        tmp->adresa = j;
        tmp->nextElement = (AK_memoryAddresses_link) malloc(sizeof(struct memoryAddresses));
        memset(tmp->nextElement, 0, sizeof (struct memoryAddresses));
        tmp = tmp->nextElement;

    }
    return OK;
}

/**
 * @author Frane Jakelić
 * @brief Method that is called in a separate thread that is responsible for acquiring locks releasing them and finding the associated block addresses
 * @todo Check multithreading, check if it's working correctly
 * @param commandArray array filled with commands that need to be secured using transactions
 * @param lengthOfArray length of commandArray
 * @param transactionId associated with the transaction
 * @return ABORT or COMMIT based on the success of the function.
 */
int AK_execute_commands(command * commandArray, int lengthOfArray) {
    int i = 0, status = 0;
    AK_memoryAddresses addresses;
    AK_memoryAddresses_link address = (AK_memoryAddresses_link) malloc(sizeof(AK_memoryAddresses_link));
    for (i = 0; i < lengthOfArray; i++) {

        if (!AK_get_memory_blocks(commandArray[i].tblName, &addresses)) {
            printf("Error reading block Addresses. Aborting\n");
            return ABORT;
        };

        address = &addresses;
        while (address->nextElement != NULL) {
            switch (commandArray[i].id_command) {
            case UPDATE:
                status = AK_acquire_lock(address->adresa, EXCLUSIVE_LOCK, pthread_self());
                break;
            case DELETE:
                status = AK_acquire_lock(address->adresa, EXCLUSIVE_LOCK, pthread_self());
                break;
            case INSERT:
                status = AK_acquire_lock(address->adresa, EXCLUSIVE_LOCK, pthread_self());
                break;
            case SELECT:
                status = AK_acquire_lock(address->adresa, SHARED_LOCK, pthread_self());
                break;
            default:
                break;
            }

            if (status == NOT_OK) {
                printf("Error acquiring lock. Aborting\n");
                AK_release_locks(&addresses, pthread_self());
                return ABORT;
            }
            address = address->nextElement;
        }
    }

    AK_command(commandArray, lengthOfArray);
    AK_release_locks(&addresses, pthread_self());
    return COMMIT;
}


/**
 * @author Frane Jakelić updated by Ivan Pusic
 * @brief thread start point all relevant functions are called from this function. It acts as an intermediary between the main thread and other threads
 * @param data transmitted to the thread from the main thread
 */
void * AK_execute_transaction(void *params) {
    int status;
    AK_transaction_data *data = (AK_transaction_data *)params;

    status = AK_execute_commands(data->array, data->lengthOfArray);
    if (status == ABORT) {
        printf("Transaction ABORTED!\n");
    } else {
        printf("Transaction COMMITED!\n");
    }
    // notify observable_transaction about transaction finish
    observable_transaction->AK_transaction_finished();

    return NULL;
}

/** 
 * @author Ivan Pusic
 * @brief Function for deleting one of active threads from array of all active transactions threads
 * @param transaction_thread Active thread to delete
 * 
 * @return Exit status (OK or NOT_OK)
 */
int AK_remove_transaction_thread(pthread_t transaction_thread) {
    int i;
    for (i = 0; i < MAX_ACTIVE_TRANSACTIONS_COUNT; ++i) {
        if(activeThreads[i] == transaction_thread) {
            activeThreads[i] = (pthread_t)NULL;
            activeTransactionsCount--;
            return OK;
        }
    }
    return NOT_OK;
}

/** 
 * @author Ivan Pusic
 * @brief Function for creating new thread. Function also adds thread ID to pthread_t array
 * @param transaction_data Data for executing transaction
 *
 * @return Exit status (OK or NOT_OK)
 */
int AK_create_new_transaction_thread(AK_transaction_data *transaction_data) {
    pthread_t t1;

    int i;
    for (i = 0; i < MAX_ACTIVE_TRANSACTIONS_COUNT; ++i) {
        if(activeThreads[i] == (pthread_t)NULL) {
            pthread_create(&t1, NULL, AK_execute_transaction, transaction_data);
            activeThreads[i] = t1;
            activeTransactionsCount++;
            return OK;
        }
    }
    return NOT_OK;
}

/**
 * @author Frane Jakelić updated by Ivan Pusic
 * @brief method that receives all the data and gives an id to that data and starts a thread that executes the transaction 
 * @param commandArray array filled with commands that need to be secured using transactions
 * @param lengthOfArray length of commandArray
 */
void AK_transaction_manager(command * commandArray, int lengthOfArray) {
    AK_transaction_data* params = malloc(sizeof(AK_transaction_data));
    params->array = malloc(sizeof(command));
    params->array = commandArray;
    params->lengthOfArray = lengthOfArray;
    transactionsCount++;

    if(activeTransactionsCount < MAX_ACTIVE_TRANSACTIONS_COUNT) {
        pthread_mutex_lock(&accessLockMutex);
        AK_create_new_transaction_thread(params);
        pthread_mutex_unlock(&accessLockMutex);
    }

    else {
        // wait until some transaction finishes 
        pthread_mutex_lock(&newTransactionLockMutex);
        AK_create_new_transaction_thread(params);
    }
}

/** 
 * @author Ivan Pusic
 * @brief Function for registering new observer of AK_observable_transaction type
 * @param observable_transaction Observable type instance
 * @param observer Observer instance
 * 
 * @return Exit status (OK or NOT_OK)
 */
int AK_transaction_register_observer(AK_observable_transaction *observable_transaction, AK_observer *observer) {
    return observable_transaction->observable->AK_register_observer(observable_transaction->observable, observer);
}

/** 
 * @author Ivan Pusic
 * @brief Function for unregistering observer from AK_observable_transction type
 * @param observable_transaction Observable type instance
 * @param observer Observer instance
 * 
 * @return Exit status (OK or NOT_OK)
 */
int AK_transaction_unregister_observer(AK_observable_transaction *observable_transaction, AK_observer *observer) {
    return observable_transaction->observable->AK_unregister_observer(observable_transaction->observable, observer);
}

/** 
 * @author Ivan Pusic
 * @brief Function for handling AK_observable_transaction notify. Function is associated to some observer instance
 * @param observer_lock Observer type instance
 */
void handle_transaction_notify(AK_observer_lock *observer_lock) {
    printf ("MESSAGE FROM TRANSACTION RECIEVED!\n");
}

/** 
 * @author Ivan Pusic
 * @brief Function for handling notify from some observable type
 * @param observer Observer type
 * @param observable Observable type
 * @param type Type of observable who sent some notice
 */
void AK_on_observable_notify(void *observer, void *observable, AK_ObservableType_Enum type) {
    switch(type) {
    case AK_TRANSACTION:
        handle_transaction_notify((AK_observer_lock*) observer);
        break;
    default:
        break;
    }
}

/** 
 * @author Ivan Pusic
 * @brief Function for handling event when some transaction is finished
 * @param transaction_thread Thread ID of transaction which is finished
 */
void AK_on_transaction_end(pthread_t transaction_thread) {
    AK_remove_transaction_thread(transaction_thread);
    //pthread_cond_broadcast(&cond_lock);
    // unlock mutex -> after this new transaction can be executed if lock stops transaction execution
    pthread_mutex_unlock(&newTransactionLockMutex);
    transactionsCount--;
    printf ("TRANSACTIN END!!!!\n");
    if(transactionsCount == 0)
        observable_transaction->AK_all_transactions_finished();
}

/** 
 * @author Ivan Pusic
 * @brief Function for handling  event when all transactions are finished
 */
void AK_on_all_transactions_end() {
    // after unlocking this mutex, main thread will continue execution
    pthread_mutex_unlock(&endTransationTestLockMutex);
    printf ("ALL TRANSACTIONS ENDED!!!\n");
}

/** 
 * @author Ivan Pusic
 * @brief Function for handling event when one of lock is released
 */
void AK_on_lock_release() {
    pthread_cond_broadcast(&cond_lock);
    printf ("TRANSACTION LOCK RELEASED!\n");
}

/** 
 * @author Ivan Pusic
 * @brief Function for handling action which is called from observable_transaction type
 * @param noticeType Type of action (event)
 */
void AK_handle_observable_transaction_action(NoticeType *noticeType) {
    switch((NoticeType)noticeType) {
    case AK_LOCK_RELEASED:
        AK_on_lock_release();
        break;
    case AK_TRANSACTION_FINISHED:
        AK_on_transaction_end(pthread_self());
        break;
    case AK_ALL_TRANSACTION_FINISHED:
        AK_on_all_transactions_end();
        break;
    default:
        break;
    }
}

/** 
 * @author Ivan Pusic
 * @brief Function which is called when lock is released
 */
void AK_lock_released() {
    observable_transaction->observable->AK_run_custom_action((NoticeType*)AK_LOCK_RELEASED);
}

/** 
 * @author Ivan Pusic
 * @brief Function which is called when some transaction is finished
 */
void AK_transaction_finished() {
    observable_transaction->observable->AK_run_custom_action((NoticeType*)AK_TRANSACTION_FINISHED);
}

/** 
 * @author Ivan Pusic
 * @brief Function which is called when all transactions are finished
 */
void AK_all_transactions_finished() {
    observable_transaction->observable->AK_run_custom_action((NoticeType*)AK_ALL_TRANSACTION_FINISHED);
}

/** 
 * @author Ivan Pusic
 * @brief Function for initialization of AK_observable_transaction type
 * 
 * @return Pointer to new AK_observable_transaction instance
 */
AK_observable_transaction * AK_init_observable_transaction() {
    observable_transaction = calloc(1, sizeof(AK_observable_transaction));
    observable_transaction->AK_transaction_register_observer = &AK_transaction_register_observer;
    observable_transaction->AK_transaction_unregister_observer = &AK_transaction_unregister_observer;
    observable_transaction->AK_lock_released = &AK_lock_released;
    observable_transaction->AK_transaction_finished = &AK_transaction_finished;
    observable_transaction->AK_all_transactions_finished = &AK_all_transactions_finished;
    observable_transaction->observable = AK_init_observable(observable_transaction, AK_TRANSACTION, &AK_handle_observable_transaction_action);
    
    return observable_transaction;
}

/** 
 * @author Ivan Pusic
 * @brief Function for initialization of AK_observer_lock type
 * 
 * @return Pointer to new AK_observer_lock instance
 */
AK_observer_lock * AK_init_observer_lock() {
    AK_observer_lock *self;
    self = calloc(1, sizeof(AK_observer_lock));
    self->observer = AK_init_observer(self, &AK_on_observable_notify);
    return self;
}

void AK_test_Transaction() {
    printf("***Test Transaction***\n");
    pthread_mutex_lock(&endTransationTestLockMutex);
    pthread_mutex_lock(&newTransactionLockMutex);
    
    AK_init_observable_transaction();
    
    memset(LockTable, 0, NUMBER_OF_KEYS * sizeof (struct transaction_list_head));

    char *tblName = "student";
    AK_list_elem row_root_insert = (AK_list_elem) malloc(sizeof (AK_list));
    Ak_Init_L(row_root_insert);
    Ak_DeleteAll_L(row_root_insert);
    int mbr, year;
    float weight;
    mbr = 38262;
    year = 2012;
    weight = 82.00;
    Ak_DeleteAll_L(row_root_insert);
    Ak_Insert_New_Element(TYPE_INT, &mbr, tblName, "mbr", row_root_insert);
    Ak_Insert_New_Element(TYPE_VARCHAR, "Ivan", tblName, "firstname", row_root_insert);
    Ak_Insert_New_Element(TYPE_VARCHAR, "Pusic", tblName, "lastname", row_root_insert);
    Ak_Insert_New_Element(TYPE_INT, &year, tblName, "year", row_root_insert);
    Ak_Insert_New_Element(TYPE_FLOAT, &weight, tblName, "weight", row_root_insert);

    AK_list_elem row_root_update = (AK_list_elem) malloc(sizeof (AK_list_elem));
    Ak_Init_L(row_root_update);
    Ak_DeleteAll_L(row_root_update);
    Ak_Insert_New_Element_For_Update(TYPE_INT, &mbr, tblName, "mbr", row_root_update, SEARCH_CONSTRAINT);
    Ak_Insert_New_Element_For_Update(TYPE_VARCHAR, "pppppppppp", tblName, "lastname", row_root_update, NEW_VALUE);

    command* komande = malloc(sizeof (command) * 2);
    komande[0].tblName = "student";
    komande[0].id_command = INSERT;
    komande[0].parameters = row_root_insert;

    komande[1].tblName = "student";
    komande[1].id_command = UPDATE;
    komande[1].parameters = row_root_update;


    
    int id_prof;
    id_prof = 35893;
    AK_list_elem row_root_p_update = (AK_list_elem) malloc(sizeof (AK_list_elem));
    Ak_Init_L(row_root_p_update);
    Ak_DeleteAll_L(row_root_p_update);
    
    Ak_Insert_New_Element_For_Update(TYPE_INT, &id_prof, "professor", "id_prof", row_root_p_update, 1);
    Ak_Insert_New_Element_For_Update(TYPE_VARCHAR, "FOI", "professor", "firstname", row_root_p_update, 0);

    command* komande1 = malloc(sizeof (command) * 1);
    komande1[0].tblName = "professor";
    komande1[0].id_command = DELETE;
    komande1[0].parameters = row_root_p_update;

    /* command* komande2 = malloc(sizeof (command) * 2); */
    /* komande2[0].tblName = "student"; */
    /* komande2[0].id_command = INSERT; */
    /* komande2[0].parameters = row_root_insert; */

    /* komande2[1].tblName = "student"; */
    /* komande2[1].id_command = UPDATE; */
    /* komande2[1].parameters = row_root_update; */

    // NOTE: This is the way on which we can broadcast notice to all observers
    // observable_transaction->observable->AK_notify_observers(observable_transaction->observable);
    AK_list *expr = (AK_list *) malloc(sizeof (AK_list));
	Ak_Init_L(expr);
	char *srcTable = "student";
	char *destTable = "selection_testt";
	int num = 2010;
    strcpy(expr->table,destTable);
	Ak_InsertAtEnd_L(TYPE_ATTRIBS, "year", sizeof ("year"), expr);
	Ak_InsertAtEnd_L(TYPE_INT, &num, sizeof (int), expr);
	Ak_InsertAtEnd_L(TYPE_OPERATOR, "<", sizeof ("<"), expr);
	Ak_InsertAtEnd_L(TYPE_ATTRIBS, "firstname", sizeof ("firstname"), expr);
	Ak_InsertAtEnd_L(TYPE_VARCHAR, "Robert", sizeof ("Robert"), expr);
	Ak_InsertAtEnd_L(TYPE_OPERATOR, "=", sizeof ("="), expr);
	Ak_InsertAtEnd_L(TYPE_OPERATOR, "OR", sizeof ("OR"), expr);
    
    command* komande_select = malloc(sizeof(command) * 1);
    komande_select[0].tblName = "student";
    komande_select[0].id_command = SELECT;
    komande_select[0].parameters = expr;

    AK_transaction_manager(komande, 2);
    //AK_transaction_manager(komande, 2);
    //AK_transaction_manager(komande1, 1);
    //AK_transaction_manager(komande_select, 1);
    
    pthread_mutex_lock(&endTransationTestLockMutex);
    
    free(observable_transaction);
    observable_transaction = NULL;
    
    printf("***End test Transaction***\n");
}