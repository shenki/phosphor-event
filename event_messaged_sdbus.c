#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stddef.h>
#include <systemd/sd-bus.h>
#include "message.H"
#include "list.h"
#include "event_messaged_sdbus.h"
#include <syslog.h>

/*****************************************************************************/
/* This set of functions are responsible for interactions with events over   */
/* dbus.  Logs come in a couple of different ways...                         */ 
/*     1) From the calls from acceptHostMessage, acceptTestMessage           */
/*     2) At startup and logs that exist alreafy are re-added                */
/*                                                                           */
/* event_record_t when loaded contain all strings and data stream for a log  */
/*                                                                           */
/* Functions naming convention                                               */
/*     prop_x    : callable dbus properties.                                 */
/*     method_x  : callable dbus functions.                                  */
/*                                                                           */
/*****************************************************************************/



sd_bus      *bus   = NULL;
sd_bus_slot *slot  = NULL;
List        *glist = NULL;

event_record_t *gCachedRec = NULL;

static int remove_log_from_dbus(Node *node);

typedef struct messageEntry_t {

	size_t         logid;
	sd_bus_slot   *messageslot;
	sd_bus_slot   *deleteslot;
	event_manager *em;

} messageEntry_t;


static void message_entry_close(messageEntry_t *m) {
	free(m);
	return;
}

static void message_entry_new(messageEntry_t **m, uint16_t logid, event_manager *em) {
	*m          = malloc(sizeof(messageEntry_t));
	(*m)->logid = logid;
	(*m)->em    = em;
	return;
}

// After calling this function the gCachedRec will be set
static event_record_t* message_record_open(event_manager *em, uint16_t logid) {

	int r = 0;
	event_record_t *rec;

	// A simple caching technique because each 
	// property needs to extract data from the
	// same data blob.  
	if (gCachedRec == NULL) {
		if (message_load_log(em, logid, &rec)) {
			gCachedRec = rec;
			return gCachedRec;
		} else 
			return NULL;
	} 

	if (logid == gCachedRec->logid) {
		r = 1;

	} else {
		message_free_log(em, gCachedRec);
		gCachedRec = NULL;

		r = message_load_log(em, logid, &rec);
		if (r)
			gCachedRec = rec;
	}

	return (r ? gCachedRec : NULL);
}



static int prop_message(sd_bus *bus,
                       const char *path,
                       const char *interface,
                       const char *property,
                       sd_bus_message *reply,
                       void *userdata,
                       sd_bus_error *error) {

	int r=0;
	messageEntry_t *m = (messageEntry_t*) userdata;
	char *p;
	struct tm *tm_info;
    char buffer[32];
    event_record_t *rec;

	rec = message_record_open(m->em, m->logid);

	if (!rec) {
		fprintf(stderr,"Warning missing evnet log for %d\n", m->logid);
		sd_bus_error_set(error,  SD_BUS_ERROR_FILE_NOT_FOUND,"Could not find log file");
        return -1;
	}

	if (!strncmp("message", property, 7))
		p = rec->message;
	else if (!strncmp("severity", property, 8))
		p = rec->severity;
	else if (!strncmp("association", property, 11))
		p = rec->association;
	else if (!strncmp("reported_by", property, 11))
		p = rec->reportedby;
	else if (!strncmp("time", property, 4)) {
		tm_info = localtime(&rec->timestamp);
		strftime(buffer, 26, "%Y:%m:%d %H:%M:%S", tm_info);
		p    = buffer;
	}
	else
		p = "";

	r = sd_bus_message_append(reply, "s", p);
	if (r < 0) {
	    fprintf(stderr,"Error building array for property %s\n", strerror(-r));
	}


	return r;
}


static int prop_message_dd(sd_bus *bus,
                       const char *path,
                       const char *interface,
                       const char *property,
                       sd_bus_message *reply,
                       void *userdata,
                       sd_bus_error *error) {

    event_record_t *rec;
    messageEntry_t *m = (messageEntry_t*) userdata;


	rec = message_record_open(m->em, m->logid);

	if (!rec) {
		sd_bus_error_set(error,  SD_BUS_ERROR_FILE_NOT_FOUND,"Could not find log file");
        return -1;
    }
	return sd_bus_message_append_array(reply, 'y', rec->p, rec->n);
}

/////////////////////////////////////////////////////////////
// Receives an array of bytes as an esel error log
// returns the messageid in 2 byte format
//  
//  S1 - Message - Simple sentence about the fail
//  S2 - Severity - How bad of a problem is this
//  S3 - Association - sensor path
//  ay - Detailed data - developer debug information
//
/////////////////////////////////////////////////////////////
static int method_accept_host_message(sd_bus_message *m,
                                      void *userdata,
                                      sd_bus_error *ret_error) {

	char *message, *severity, *association, *s;
	size_t   n = 4;
	uint8_t *p;
	int r;
	uint16_t logid;
	event_record_t rec;
	event_manager *em = (event_manager *) userdata;

	r = sd_bus_message_read(m, "sss", &message, &severity, &association);
	if (r < 0) {
		fprintf(stderr, "Failed to parse the String parameter: %s\n", strerror(-r));
		return r;
	}

	r = sd_bus_message_read_array(m, 'y', (const void **)&p, &n);
	if (r < 0) {
		fprintf(stderr, "Failed to parse the array of bytes parameter: %s\n", strerror(-r));
		return r;
	}

    rec.message     = (char*) message;
    rec.severity    = (char*) severity;
    rec.association = (char*) association;
    rec.reportedby  = (char*) "Host";
    rec.p           = (uint8_t*) p;
    rec.n           = n;

    asprintf(&s, "%s %s (%s)", rec.severity, rec.message, rec.association);
	syslog(LOG_NOTICE, s);
	free(s);

	logid = message_create_new_log_event(em, &rec);

	if (logid) 
		r = send_log_to_dbus(em, logid);

	return sd_bus_reply_method_return(m, "q", logid);
}


static int method_accept_test_message(sd_bus_message *m,
                                      void *userdata,
                                      sd_bus_error *ret_error) {

	//  Random debug data including, ascii, null, >signed int, max
	uint8_t p[] = {0x30, 0x00, 0x13, 0x7F, 0x88, 0xFF};
	char *s;
	uint16_t logid;
	event_record_t rec;
	event_manager *em = (event_manager *) userdata;

	rec.message     = (char*) "A Test event log just happened";
	rec.severity    = (char*) "Info";
	rec.association = (char*) "/org/openbmc/inventory/system/chassis/motherboard/dimm3 " \
	                          "/org/openbmc/inventory/system/chassis/motherboard/dimm2";
	rec.reportedby  = (char*) "Test";
	rec.p           = (uint8_t*) p;
	rec.n           = 6;


	asprintf(&s, "%s %s (%s)", rec.severity, rec.message, rec.association);
	syslog(LOG_NOTICE, s);
	free(s);

	logid = message_create_new_log_event(em, &rec);
	send_log_to_dbus(em, logid);

	return sd_bus_reply_method_return(m, "q", logid);
}


static int method_clearall(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	Node *n;
	messageEntry_t *p;
	
	// This deletes one log at a time and seeing how the
	// list shrinks using the NULL works fine here
	while (n = list_get_next_node(glist, NULL)) {
		p = (messageEntry_t *) n->data;
		message_delete_log(p->em, p->logid);
		remove_log_from_dbus(n);
	}

	return sd_bus_reply_method_return(m, "q", 0);
}


static int method_deletelog(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {

	Node *n = (Node*)userdata;
	messageEntry_t *p = (messageEntry_t *) n->data;

	message_delete_log(p->em, p->logid);
	remove_log_from_dbus(n);
	return sd_bus_reply_method_return(m, "q", 0);
}



static const sd_bus_vtable recordlog_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("acceptHostMessage", "sssay", "q", method_accept_host_message, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("acceptTestMessage", NULL, "q", method_accept_test_message, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("clear", NULL, "q", method_clearall, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable log_vtable[] = {
	SD_BUS_VTABLE_START(0),   
	SD_BUS_PROPERTY("association",      "s", prop_message, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("message",          "s", prop_message, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("severity",         "s", prop_message, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("reported_by",      "s", prop_message, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("time",             "s", prop_message, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("debug_data", "ay", prop_message_dd ,0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};


static const sd_bus_vtable recordlog_delete_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("delete", NULL, "q", method_deletelog, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static int remove_log_from_dbus(Node *node) {

	messageEntry_t *p = (messageEntry_t *) node->data;
	int r;
	char buffer[32];

	snprintf(buffer, sizeof(buffer), "/org/openbmc/records/events/%d", p->logid);

	printf("Attempting to delete %s\n", buffer);

	r = sd_bus_emit_object_removed(bus, buffer);
	if (r < 0) {
		fprintf(stderr, "Failed to emit the delete signal %s\n", strerror(-r));
		return -1;
	}	
	sd_bus_slot_unref(p->messageslot);
	sd_bus_slot_unref(p->deleteslot);

	message_entry_close(p);
	list_delete_node(glist, node);

	return 0;
}

int send_log_to_dbus(event_manager *em, const uint16_t logid) {

	char loglocation[64];
	int r;
	messageEntry_t *m;
	Node *node;


	snprintf(loglocation, sizeof(loglocation), "/org/openbmc/records/events/%d", logid);

	message_entry_new(&m, logid, em);

	node = list_add_node(glist, m);

	r = sd_bus_add_object_vtable(bus,
	                             &m->messageslot,
	                             loglocation,
	                             "org.openbmc.record",
	                             log_vtable,
	                             m);
	if (r < 0) {
		fprintf(stderr, "Failed to acquire service name: %s %s\n", loglocation, strerror(-r));
		message_entry_close(m);
		list_delete_last_node(glist);
		return 0;
	}

	r = sd_bus_add_object_vtable(bus,
	                             &m->deleteslot,
	                             loglocation,
	                             "org.openbmc.Object.Delete",
	                             recordlog_delete_vtable,
	                             node);
	
	printf("Event Log added %s\n", loglocation);

	r = sd_bus_emit_object_added(bus, loglocation);
	if (r < 0) {
		fprintf(stderr, "Failed to emit signal %s\n", strerror(-r));
		return 0;
	}

	return logid;
}


int start_event_monitor(void) {

	int r;

	for (;;) {

		r = sd_bus_process(bus, NULL);
		if (r < 0) {
			fprintf(stderr, "Failed to process bus: %s\n", strerror(-r));
			break;
		}

		if (r > 0)
			continue;

		r = sd_bus_wait(bus, (uint64_t) -1);
		if (r < 0) {
			fprintf(stderr, "Failed to wait on bus: %s\n", strerror(-r));
			break;
		}
	}

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}


/* Only thing we are doing in this function is to get a connection on the dbus */
int build_bus(event_manager *em) {

	int r = 0;

	glist = list_create();
	/* Connect to the system bus */
	r = sd_bus_open_system(&bus);
	if (r < 0) {
		fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
		goto finish;
	}

	/* Install the object */
	r = sd_bus_add_object_vtable(bus,
	                             &slot,
	                             "/org/openbmc/records/events",  /* object path */
	                             "org.openbmc.recordlog",   /* interface name */
	                             recordlog_vtable,
	                             em);
	if (r < 0) {
		fprintf(stderr, "Failed to issue method call: %s\n", strerror(-r));
		goto finish;
	}

	/* Take a well-known service name so that clients can find us */
	r = sd_bus_request_name(bus, "org.openbmc.records.events", 0);
	if (r < 0) {
		fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-r));
	}	
	
	/* You want to add an object manager to support deleting stuff  */
	/* without it, dbus can show interfaces that no longer exist */
	r = sd_bus_add_object_manager(bus, NULL, "/org/openbmc/records/events");
	if (r < 0) {
		fprintf(stderr, "Object Manager failure  %s\n", strerror(-r));
	}


	finish:
	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

void cleanup_event_monitor(void) {
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);
}