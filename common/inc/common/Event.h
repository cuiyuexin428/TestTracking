#pragma once

namespace common {

// Base class for all events. Subclasses implement the action that runs
// when the event is dispatched by an EventManager.
class Event {
public:
    virtual ~Event() = default;

    // Executes the event's action. Must be implemented by subclasses.
    virtual void execute() = 0;
};

}  // namespace common
