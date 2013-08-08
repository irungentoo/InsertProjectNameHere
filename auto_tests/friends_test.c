/* Unit testing for friend requests, statuses, and messages.
 *  Purpose: Check that messaging functions actually do what
 *          they're supposed to by setting up two local clients.
 *
 *  Design: (Subject to change.)
 *      1. Parent sends a friend request, and waits for a response.
 *          It it doesn't get one, it kills the child.
 *      2. Child gets friend request, accepts, then waits for a status change.
 *      3. The parent waits on a status change, killing the child if it takes
 *          too long.
 *      4. The child gets the status change, then sends a message. After that,
 *          it returns. If if doesn't get the status change, it just loops forever.
 *      5. After getting the status change, the parent waits for a message, on getting
 *          one, it waits on the child to return, then returns 0.
 *
 *  Note about "waiting":
 *      Wait time is decided by WAIT_COUNT and WAIT_TIME. c_sleep(WAIT_TIME) WAIT_COUNT
 *      times. This is used both to ensure that we don't loop forever on a broken build,
 *      and that we don't get too slow with messaging. The current time is 15 seconds. */

#include "../core/friend_requests.h"
#include "../core/Messenger.h"
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/wait.h>

#define WAIT_COUNT 30
#define WAIT_TIME 500

/* first step, second step */
#define FIRST_FLAG 0x1
#define SECOND_FLAG 0x2

/* ensure that we sleep in milliseconds */
#ifdef WIN32
#define c_sleep(x) Sleep(x)
#else
#define c_sleep(x) usleep(1000*x)
#endif

uint8_t *parent_id = NULL;
uint8_t *child_id = NULL;

pid_t child_pid = 0;
int request_flags = 0;

void do_tox(void)
{
    static int dht_on = 0;

    if(!dht_on && DHT_isconnected()) {
        dht_on = 1;
    } else if(dht_on && !DHT_isconnected()) {
        dht_on = 0;
    }

    doMessenger();
}

void parent_confirm_message(int num, uint8_t *data, uint16_t length)
{
    puts("OK");
    request_flags |= SECOND_FLAG;
}

void parent_confirm_status(int num, uint8_t *data, uint16_t length)
{
    puts("OK");
    request_flags |= FIRST_FLAG;
}

int parent_friend_request(void)
{
    char *message = "Watson, come here, I need you.";
    int len = strlen(message);
    int i = 0;

    fputs("Sending child request.", stdout);
    fflush(stdout);

    m_addfriend(child_id, (uint8_t *)message, len);

    /* wait on the status change */
    for(i = 0; i < WAIT_COUNT; i++) {
        do_tox();
        if(request_flags & FIRST_FLAG)
            break;
        fputs(".", stdout);
        fflush(stdout);
        c_sleep(WAIT_TIME);
    }

    if(!(request_flags & FIRST_FLAG)) {
        fputs("\nfriends_test: The child took to long to respond!\n"
              "Friend requests may be broken, failing build!\n", stderr);
        kill(child_pid, SIGKILL);
        return -1;
    }

    return 0;
}

void child_got_request(uint8_t *public_key, uint8_t *data, uint16_t length)
{
    fputs("OK\nsending status to parent", stdout);
    fflush(stdout);
    m_addfriend_norequest(public_key);
    request_flags |= FIRST_FLAG;
}

void child_got_statuschange(int friend_num, uint8_t *string, uint16_t length)
{
    request_flags |= SECOND_FLAG;
}

int parent_wait_for_message(void)
{
    int i = 0;

    fputs("Parent waiting for message.", stdout);
    fflush(stdout);

    for(i = 0; i < WAIT_COUNT; i++) {
        do_tox();
        if(request_flags & SECOND_FLAG)
            break;
        fputs(".", stdout);
        fflush(stdout);
        c_sleep(WAIT_TIME);
    }

    if(!(request_flags & SECOND_FLAG)) {
        fputs("\nParent hasn't recieved the message yet!\n"
              "Messaging may be broken, failing the build!\n", stderr);
        kill(child_pid, SIGKILL);
        return -1;
    }

    return 0;
}

void cleanup(void)
{
    munmap(parent_id, crypto_box_PUBLICKEYBYTES);
    munmap(child_id, crypto_box_PUBLICKEYBYTES);
    puts("============= END TEST =============");
}

int main(int argc, char *argv[])
{
    puts("=========== FRIENDS_TEST ===========");

    /* set up the global memory */
    parent_id = mmap(NULL, crypto_box_PUBLICKEYBYTES, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    child_id = mmap(NULL, crypto_box_PUBLICKEYBYTES, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    fputs("friends_test: Starting test...\n", stdout);
    if((child_pid = fork()) == 0) {
        /* child */
        int i = 0;
        char *message = "Y-yes Mr. Watson?";

        initMessenger();
        Messenger_save(child_id);
        msync(child_id, crypto_box_PUBLICKEYBYTES, MS_SYNC);

        m_callback_friendrequest(child_got_request);
        m_callback_statusmessage(child_got_statuschange);

        /* wait on the friend request */
       while(!(request_flags & FIRST_FLAG))
            do_tox();

        /* wait for the status change */
        while(!(request_flags & SECOND_FLAG))
            do_tox();

        for(i = 0; i < 6; i++) {
            /* send the message six times, just to be sure */
            m_sendmessage(0, (uint8_t *)message, strlen(message));
            do_tox();
        }

        return 0;
    }

    /* parent */
    if(atexit(cleanup) != 0) {
        fputs("friends_test: atexit() failed!\nFailing build...\n", stderr);
        kill(child_pid, SIGKILL);
        return -1;
    }

    msync(parent_id, crypto_box_PUBLICKEYBYTES, MS_SYNC);
    m_callback_statusmessage(parent_confirm_status);
    m_callback_friendmessage(parent_confirm_message);

    /* hacky way to give the child time to set up */
    c_sleep(50);

    initMessenger();
    Messenger_save(parent_id);

    if(parent_friend_request() == -1)
        return -1;

    if(parent_wait_for_message() == -1)
        return -1;

    wait(NULL);
    fputs("friends_test: Build passed!\n", stdout);
    return 0;
}
