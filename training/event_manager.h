#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 1. Define the Handler signature
/**
 * @typedef EventHandler
 * @brief "Function pointer" type for handling events.
 *
 * syntax: return_type (*pointer_name)(parameter_types);
 * This type defines a pointer to a function that handles events.
 * The handler function receives the name of the event as a string (eventName)
 * and a pointer to a context-specific data structure (context).
 *
 * @param eventName The name of the event being handled.
 * @param context   A pointer to user-defined data associated with the event.
 */
typedef void (*EventHandler)(int eventId, const char* eventName, void* context);

// 2. Module Structure
typedef struct {
    char name[50]; // module name
    EventHandler handleEvent; // how each module handle the resp event differs
    // NOTE: this is a common event handler, 
    // which needs to have seperate blocks of logic for different types of event 
    // (eventName passed as arg to the handler itself)
} Module;

// 3. Subscriber Node 
// (Each event has a linked list of subscribers, 
// subscribers are nothing but different modules subscribing to the event)
typedef struct SubNode {
    Module* module;
    struct SubNode* next;
} Subscriber;

// 4. Event Structure
typedef struct {
    char name[50]; // event name
    Subscriber* subs; // subscribers linked list
} Event;

// 4. Event Manager Structure
typedef struct {
    Event* events; // Array of linked lists
    int maxEvents;
} EventManager;

// --- Event Manager Functions ---
// since C dont have class like encapsulation, we internally try to formulate in such manner
// assume declaration similar to python like self of class object as part of function's arg...

int find_event_id(EventManager* self, const char* eventName) {
    for(int i=0; i<self->maxEvents; i++) {
        if (strcmp(self->events[i].name, eventName) == 0) {
            return i;
        }
    }
    return -1;
}

// C doesnt support polymorphism / fn overloading, fn names are required to be unique

void register_module_by_name(EventManager* self, const char* eventName, Module* mod) {
    register_module_by_id(self, find_event_id(self, eventName), mod);
}

void register_module_by_id(EventManager* self, int eventId, Module* mod) {
    if (eventId < 0 || eventId >= self->maxEvents) return;

    Subscriber* newSub = (Subscriber*)malloc(sizeof(Subscriber));
    newSub->module = mod; // append new sub on top of linked list, T=O(1)
    newSub->next = self->events[eventId].subs;
    self->events[eventId].subs = newSub;
    printf("||>> Module \"%s\" registered >>> event[%d]: %s >>||\n", mod->name, eventId, self->events[eventId].name);
}

void unregister_module_by_name(EventManager* self, const char* eventName, Module* mod) {
    unregister_module_by_id(self, find_event_id(self, eventName), mod);
}

void unregister_module_by_id(EventManager* self, int eventId, Module* mod) {
    if (eventId < 0 || eventId >= self->maxEvents) return;

    Subscriber* iterSub = self->events[eventId].subs;
    if (iterSub != NULL && iterSub->module == mod) {
        self->events[eventId].subs = iterSub->next;
        free(iterSub);
    }
    else {
        while (iterSub->next != NULL) {
            if (iterSub->next->module == mod) {
                Subscriber* temp = iterSub->next;
                iterSub->next = temp->next;
                free(temp);
                break;
            }
        }
    }
    // linked list --> search and remove, T=O(n)    
    printf("||<< Module %s unregistered <<< event[%d]: %s <<||\n", mod->name, eventId, self->events[eventId].name);
}

// note: a module might register for a same event multiple times, 
// since we aren't checking if the module is already registered in the existing list of subscriber while registering
// its the module's responsibility to keep track of such stuff

void trigger_event_by_name(EventManager* self, const char* eventName, void* context) {
    trigger_event_by_id(self, find_event_id(self, eventName), context);
}

void trigger_event_by_id(EventManager* self, int eventId, void* context) {
    const char* eventName = self->events[eventId].name;
    printf("\n===== Triggering Event[ID:%d] \"%s\" =====\n", eventId, eventName);
    
    Subscriber* current = self->events[eventId].subs;
    if (current == NULL) {
        printf("No modules registered for this event.\n");
        return;
    }

    while (current != NULL) {
        printf("--> Notifying Module: %s <--\n", current->module->name);
        current->module->handleEvent(eventId, eventName, context);
        current = current->next;
    }
}

// note: in typical applications, handle-event's context is provided by the event for getting additional information
// on the changes that have occure, its not taken from external as param, to keep things simple, we are using from fn's arg directly

// Init Fn()
EventManager* create_manager(int numEvents) {
    EventManager* em = (EventManager*)malloc(sizeof(EventManager));
    em->maxEvents = numEvents;
    em->events = (Event*)calloc(numEvents, sizeof(Event));
    return em;
}