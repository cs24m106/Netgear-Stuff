#include "event_manager.h"
#include <time.h>

typedef enum {
    EVENT_USER_LOGIN = 0,
    EVENT_PAYMENT_SUCCESS,
    EVENT_SYSTEM_LOGOUT,
    MAX_EVENTS // Traditional trick: automatically holds the count of events
} EventType;

// String mapping: Must match the order of the enum above
const char* EVENT_NAMES[] = {
    "U_LOGIN",
    "PAYMENT",
    "S_LOGOT"
};


// --- 1. Various Context Structs ---

typedef struct {
    char username[32];
    int loginAttempts;
} LoginContext;

typedef struct {
    float amount;
    char currency[8];
    char transactionId[16];
} PaymentContext;


// --- 2. Concrete Module Handlers ---

void audit_logic(int eventId, const char* eventName, void* ctx) {
    time_t now; time(&now);
    char* t = ctime(&now);
    t[strlen(t)-1] = '\0'; // Remove newline from ctime
    printf("[Audit] [%s] Action[%d]: %s triggered.\n", t, eventId, eventName);
}

void security_logic(int eventId, const char* eventName, void* ctx) {
    if (eventId == EVENT_USER_LOGIN) {
        LoginContext* data = (LoginContext*)ctx;
        data->loginAttempts++;
        printf("[Security] Verifying credentials for user: %s for %d-login attempt.\n", data->username, data->loginAttempts);
    }
}

void billing_logic(int eventID, const char* eventName, void* ctx) {
    if (eventID == EVENT_PAYMENT_SUCCESS) {
        PaymentContext* data = (PaymentContext*)ctx;
        printf("[Billing] Generating invoice for id:%s for %.2f %s\n", data->transactionId, data->amount, data->currency);
    }
}

static int analytic_counter[MAX_EVENTS] = {0};
void analytics_logic(int eventId, const char* eventName, void* ctx) {
    printf("[Analytics] Incrementing counter for %s.", eventName);
    analytic_counter[eventId]++;
    printf(" Updated [event:count] --> ");
    for (int i=0; i<MAX_EVENTS; i++)
        printf("%7s:%d, ", EVENT_NAMES[i], analytic_counter[i]);
    printf("\n");
}

// --- 3. Application Main ---

int main() {
    // Initialize Manager with 3 Events
    EventManager* em = create_manager(MAX_EVENTS);
    // Map Enum Strings to the Manager's internal Event structs
    for (int i = 0; i < MAX_EVENTS; i++) {
        strcpy(em->events[i].name, EVENT_NAMES[i]);
    }

    // Define 4 Modules
    Module modAudit = {"AuditModule", audit_logic};
    Module modSecurity = {"SecurityModule", security_logic};
    Module modBilling = {"BillingModule", billing_logic};
    Module modAnalytics = {"AnalyticsModule", analytics_logic};

    // Subscriptions
    // here audit & analytics modules subscribe to all events above
    // security module subs only to login and billing module subs only to payment

    register_module_by_id(em, EVENT_USER_LOGIN, &modAudit);
    register_module_by_id(em, EVENT_PAYMENT_SUCCESS, &modAudit);
    register_module_by_id(em, EVENT_SYSTEM_LOGOUT, &modAudit);

    register_module_by_id(em, EVENT_USER_LOGIN, &modSecurity);
    
    register_module_by_id(em, EVENT_PAYMENT_SUCCESS, &modBilling);
    
    register_module_by_id(em, EVENT_USER_LOGIN, &modAnalytics);
    register_module_by_id(em, EVENT_PAYMENT_SUCCESS, &modAnalytics);
    register_module_by_id(em, EVENT_SYSTEM_LOGOUT, &modAnalytics);
    

    // --- CASE 1: A User Logs In (multiple attempts) ---
    LoginContext user1 = {"Alice_99", 0};
    trigger_event_by_id(em, EVENT_USER_LOGIN, &user1); // efficient
    unregister_module_by_id(em, EVENT_USER_LOGIN, &modAnalytics); // remove analytics observer from login event for testing
    trigger_event_by_id(em, EVENT_USER_LOGIN, &user1);
    trigger_event_by_name(em, EVENT_NAMES[EVENT_USER_LOGIN], &user1); // extra fn-ality

    // --- CASE 2: A Payment Happens ---
    PaymentContext purchase = {125.50, "USD", "TXN-77821"};
    trigger_event_by_id(em, EVENT_PAYMENT_SUCCESS, &purchase);

    // --- CASE 3: System Logout ---
    trigger_event_by_id(em, EVENT_SYSTEM_LOGOUT, NULL);
    return 0;
}