#include "resp.h"

// RESP Format: *<length of array>\r\n$<length of string>\r\n<string>\r\n...
// Example: "*3\r\n$3\r\nSET\r\n$1\r\nK\r\n$1\r\nV\r\n"
// This means there are 3 arguments in the array.
// The first argument is a string of length 3, which is "SET".
// The second argument is a string of length 1, which is "K".
// The third argument is a string of length 1, which is "V".
// The command is "SET K V".
// The arguments are "K" and "V".
// The command is "SET K V".

std::optional<Command> parseCommand(const std::string& data) {
    Command command;
    if (data.empty() || data[0] != '*') {
        return std::nullopt; // If the data is empty or the first character is not '*', return nullopt.
    }

    int lengthArray = std::stoi(data.substr(1, data.find('\r') - 1));
    // Extract the length of the array from the data.
    int position = data.find('\r') + 2;
    // Extract the position of the first argument from the data.

    for (int i = 0; i < lengthArray; i++) {
        // Extract the argument from the data.
        if (data[position] != '$') {
            return std::nullopt;
        }
        position++;
        // Extract the position of the string from the data.
        int positionString = data.find('\r', position) + 2;
        // Extract the length of the string from the data.
        int lengthString = std::stoi(data.substr(position, positionString - position - 1));
        // Extract the string from the data.
        std::string argument = data.substr(positionString, lengthString);
        position = positionString + lengthString + 2;
        // If the argument is the first argument, set the name of the command.
        // Otherwise, add the argument to the list of arguments.
        if (i == 0) {
            command.name = argument;
        }
        else {
            command.args.push_back(argument);
        }
    }
    // Return the command.
    return std::make_optional(command);
}