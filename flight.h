#ifndef FLIGHT_H
#define FLIGHT_h
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>
#include <pthread.h>

typedef struct Seat{
    int seat_number;
    bool is_booked;
    char passenger_name[100];
} Seat;

typedef struct Flight{
    char flight_number[10];
    char origin[50];
    char destination[50];
    char date[10];
    char time[20];
    Seat seats[100];
    pthread_mutex_t seat_locks[100]; // Mutex locks for each seat
    sem_t booking_semaphore; // Semaphore for controlling access to seat bookings
} Flight;

#endif 