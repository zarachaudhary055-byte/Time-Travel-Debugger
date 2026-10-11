// Time-Travel Debugger - Server (Phase 01)
//
// The server works in 4 passes:
//   Pass 0 : check that the program is written correctly (validate)
//   Pass 1 : write resolve.bin and put the address of each called function
//            inside every "call" line (resolve)
//   Pass 2 : run the program line by line and take a snapshot after each step
//   Pass 3 : save all the snapshots in session.tdbg
 
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace std;
 
const int MAX_VARS = 16;      // variables in one function
const int MAX_DEPTH = 64;     // functions inside each other (call stack size)
const int MAX_FUNCS = 128;    // functions in one program
const int MAX_CALLS = 512;    // call lines in one program
const int MAX_WORDS = 18;     // words in one line
const long long MAX_FILE_SIZE = 15LL * 1024 * 1024;   // 15 MB
 
 
// ---------------------------------------------------------------
// Basic structures
// ---------------------------------------------------------------
 
struct Variable
{
    string name;
    int value;
};
 
// One function call on the call stack
struct Frame
{
    string funcName;
    int argCount;
    Variable args[MAX_VARS];
    int returnPos;              // position in resolve.bin to go back to
    Variable locals[MAX_VARS];
    int localCount;
 
    Frame()
    {
        argCount = 0;
        returnPos = -1;
        localCount = 0;
    }
};
 
// A picture of the whole call stack at one moment
struct Snapshot
{
    Frame* frames;
    int depth;
 
    Snapshot()
    {
        frames = nullptr;
        depth = 0;
    }
 
    ~Snapshot()
    {
        delete[] frames;
    }
 
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
};
 
 
// ---------------------------------------------------------------
// Stack (made with a linked list)
// ---------------------------------------------------------------
 
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
 
    Node* top;
    int count;
 
public:
    Stack()
    {
        top = nullptr;
        count = 0;
    }
 
    ~Stack()
    {
        while (!isEmpty())
        {
            pop();
        }
    }
 
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;
 
    void push(const T& item)
    {
        if (count >= MAX_DEPTH)
        {
            return;
        }
 
        Node* node = new Node;
        node->data = item;
        node->next = top;
        top = node;
        count++;
    }
 
    T pop()
    {
        if (top == nullptr)
        {
            return T();
        }
 
        Node* old = top;
        T item = old->data;
        top = old->next;
        delete old;
        count--;
        return item;
    }
 
    T& peek()
    {
        return top->data;
    }
 
    bool isEmpty()
    {
        return top == nullptr;
    }
 
    int size()
    {
        return count;
    }
 
    // Copy the stack into an array, top first. Returns how many were copied.
    int copyTo(T out[], int maxCount)
    {
        int n = 0;
        Node* cur = top;
 
        while (cur != nullptr && n < maxCount)
        {
            out[n] = cur->data;
            n++;
            cur = cur->next;
        }
 
        return n;
    }
};
 
 
// ---------------------------------------------------------------
// Timeline (made with a doubly linked list)
// ---------------------------------------------------------------
 
struct TimeNode
{
    Snapshot* snap;
    TimeNode* next;
    TimeNode* prev;
};
 
class Timeline
{
    TimeNode* head;
    TimeNode* tail;
    int count;
 
public:
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        count = 0;
    }
 
    ~Timeline()
    {
        TimeNode* cur = head;
 
        while (cur != nullptr)
        {
            TimeNode* next = cur->next;
            delete cur->snap;
            delete cur;
            cur = next;
        }
    }
 
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;
 
    // Add a snapshot at the end
    void add(Snapshot* snap)
    {
        TimeNode* node = new TimeNode;
        node->snap = snap;
        node->next = nullptr;
        node->prev = tail;
 
        if (head == nullptr)
        {
            head = node;
        }
        else
        {
            tail->next = node;
        }
 
        tail = node;
        count++;
    }
 
    TimeNode* first()
    {
        return head;
    }
 
    int size()
    {
        return count;
    }
};
 
 
// ---------------------------------------------------------------
// Helper functions for reading words and files
// ---------------------------------------------------------------
 
// Read the next line that is not empty. Removes the \r of Windows files.
bool readLine(ifstream& in, string& line)
{
    string temp;
 
    while (getline(in, temp))
    {
        if (!temp.empty() && temp.back() == '\r')
        {
            temp.pop_back();
        }
 
        if (!temp.empty())
        {
            line = temp;
            return true;
        }
    }
 
    return false;
}
 
// Get word number n of a line (first word is 0). Returns "" if there is none.
string wordAt(const string& line, int n)
{
    int pos = 0;
    int size = line.size();
 
    for (int i = 0; i <= n; i++)
    {
        while (pos < size && (line[pos] == ' ' || line[pos] == '\t'))
        {
            pos++;
        }
 
        if (pos >= size)
        {
            return "";
        }
 
        int start = pos;
 
        while (pos < size && line[pos] != ' ' && line[pos] != '\t')
        {
            pos++;
        }
 
        if (i == n)
        {
            return line.substr(start, pos - start);
        }
    }
 
    return "";
}
 
// Split a line into words. Returns the number of words, or -1 if too many.
int splitWords(const string& line, string words[], int maxWords)
{
    int count = 0;
 
    while (true)
    {
        string w = wordAt(line, count);
 
        if (w.empty())
        {
            break;
        }
 
        if (count >= maxWords)
        {
            return -1;
        }
 
        words[count] = w;
        count++;
    }
 
    return count;
}
 
bool writeBytes(FILE* f, const void* data, size_t size)
{
    return fwrite(data, 1, size, f) == size;
}
 
bool readBytes(FILE* f, void* data, size_t size)
{
    return fread(data, 1, size, f) == size;
}
 
bool writeInt(FILE* f, int value)
{
    return writeBytes(f, &value, sizeof(int));
}
 
// Text is saved as [size : 4 bytes][letters]
bool writeText(FILE* f, const string& text)
{
    unsigned int size = text.size();
 
    if (!writeBytes(f, &size, sizeof(size)))
    {
        return false;
    }
 
    return writeBytes(f, text.data(), size);
}
 
 
// ---------------------------------------------------------------
// Pass 0 : validate
// ---------------------------------------------------------------
 
bool validateProgram(const char* sourceFile)
{
    // check the file size
    ifstream sizeCheck(sourceFile, ios::binary | ios::ate);
 
    if (!sizeCheck.is_open())
    {
        return false;
    }
 
    long long fileSize = sizeCheck.tellg();
    sizeCheck.close();
 
    if (fileSize < 0 || fileSize > MAX_FILE_SIZE)
    {
        return false;
    }
 
    ifstream in(sourceFile);
 
    if (!in.is_open())
    {
        return false;
    }
 
    string line;
    bool insideFunc = false;
    int funcCount = 0;
 
    while (readLine(in, line))
    {
        string word = wordAt(line, 0);
 
        if (word == "func")
        {
            // a function inside a function is not allowed
            if (insideFunc)
            {
                return false;
            }
 
            // a function must have a name
            if (wordAt(line, 1).empty())
            {
                return false;
            }
 
            insideFunc = true;
            funcCount++;
        }
        else if (word == "func_end")
        {
            // func_end without a func
            if (!insideFunc)
            {
                return false;
            }
 
            insideFunc = false;
        }
    }
 
    // a function was not closed
    if (insideFunc)
    {
        return false;
    }
 
    // we need at least one function
    if (funcCount == 0)
    {
        return false;
    }
 
    return true;
}
 
 
// ---------------------------------------------------------------
// Pass 1 : resolve
//
// Every line is saved in resolve.bin like this:
//   [position : 8 bytes][text size : 4 bytes][text]
// For a normal line the position is just its own place in the file.
// For a "call" line we put the place of the called function there.
// ---------------------------------------------------------------
 
// Writes one line to resolve.bin. Returns where the record starts, or -1.
long long writeRecord(FILE* f, const string& text)
{
    long long pos = ftell(f);
 
    if (pos < 0)
    {
        return -1;
    }
 
    if (!writeBytes(f, &pos, sizeof(pos)))
    {
        return -1;
    }
 
    if (!writeText(f, text))
    {
        return -1;
    }
 
    return pos;
}
 
// Reads one record from resolve.bin. Returns its position field, or -1.
long long readRecord(FILE* f, string& text)
{
    long long pos;
    unsigned int size;
 
    if (!readBytes(f, &pos, sizeof(pos)))
    {
        return -1;
    }
 
    if (!readBytes(f, &size, sizeof(size)))
    {
        return -1;
    }
 
    text.clear();
 
    if (size > 0)
    {
        text.resize(size);
 
        if (!readBytes(f, &text[0], size))
        {
            text.clear();
            return -1;
        }
    }
 
    return pos;
}
 
// Returns the place of "func main" in resolve.bin, or -1 if something is wrong.
long long resolveProgram(const char* sourceFile, const char* resolveFile)
{
    string funcNames[MAX_FUNCS];
    long long funcPos[MAX_FUNCS];
    int funcCount = 0;
 
    long long callPos[MAX_CALLS];
    string callTarget[MAX_CALLS];
    int callCount = 0;
 
    ifstream source(sourceFile);
 
    if (!source.is_open())
    {
        return -1;
    }
 
    FILE* out = fopen(resolveFile, "wb+");
 
    if (out == nullptr)
    {
        return -1;
    }
 
    long long mainPos = -1;
    string line;
 
    // first round: write all lines and remember the functions and calls
    while (readLine(source, line))
    {
        long long pos = writeRecord(out, line);
 
        if (pos < 0)
        {
            fclose(out);
            return -1;
        }
 
        string word = wordAt(line, 0);
 
        if (word == "func")
        {
            string name = wordAt(line, 1);
 
            if (name.empty() || funcCount >= MAX_FUNCS)
            {
                fclose(out);
                return -1;
            }
 
            // two functions with the same name are not allowed
            for (int i = 0; i < funcCount; i++)
            {
                if (funcNames[i] == name)
                {
                    fclose(out);
                    return -1;
                }
            }
 
            funcNames[funcCount] = name;
            funcPos[funcCount] = pos;
            funcCount++;
 
            if (name == "main")
            {
                mainPos = pos;
            }
        }
        else if (word == "call")
        {
            string name = wordAt(line, 1);
 
            if (name.empty() || callCount >= MAX_CALLS)
            {
                fclose(out);
                return -1;
            }
 
            callPos[callCount] = pos;
            callTarget[callCount] = name;
            callCount++;
        }
    }
 
    // the program needs a main function
    if (mainPos < 0)
    {
        fclose(out);
        return -1;
    }
 
    // second round: put the function address inside every call line
    for (int i = 0; i < callCount; i++)
    {
        long long target = -1;
 
        for (int j = 0; j < funcCount; j++)
        {
            if (funcNames[j] == callTarget[i])
            {
                target = funcPos[j];
                break;
            }
        }
 
        // calling a function that does not exist
        if (target < 0)
        {
            fclose(out);
            return -1;
        }
 
        if (fseek(out, callPos[i], SEEK_SET) != 0)
        {
            fclose(out);
            return -1;
        }
 
        if (!writeBytes(out, &target, sizeof(target)))
        {
            fclose(out);
            return -1;
        }
    }
 
    fclose(out);
    return mainPos;
}
 
 
// ---------------------------------------------------------------
// Pass 2 : run the program
// ---------------------------------------------------------------
 
// Take a picture of the call stack and add it to the timeline
void takeSnapshot(Stack<Frame>& stack, Timeline& timeline)
{
    Snapshot* snap = new Snapshot;
 
    snap->depth = stack.size();
    snap->frames = new Frame[snap->depth];
    stack.copyTo(snap->frames, snap->depth);
 
    timeline.add(snap);
}
 
// Find a variable in a frame. Locals are checked first, then arguments.
Variable* findVariable(Frame& frame, const string& name)
{
    for (int i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            return &frame.locals[i];
        }
    }
 
    for (int i = 0; i < frame.argCount; i++)
    {
        if (frame.args[i].name == name)
        {
            return &frame.args[i];
        }
    }
 
    return nullptr;
}
 
// Shows an error, closes the file and returns false
bool runtimeError(const char* message, FILE* f)
{
    cerr << "Runtime error: " << message << endl;
    fclose(f);
    return false;
}
 
// Returns true if the program ran to the end, false if an error stopped it.
bool executeProgram(const char* resolveFile, long long mainPos, Timeline& timeline)
{
    Stack<Frame> stack;
 
    // start with the main function on the stack
    Frame mainFrame;
    mainFrame.funcName = "main";
    stack.push(mainFrame);
    takeSnapshot(stack, timeline);
 
    FILE* f = fopen(resolveFile, "rb");
 
    if (f == nullptr)
    {
        cerr << "Runtime error: Could not open resolve.bin." << endl;
        return false;
    }
 
    // go to the "func main" line
    if (fseek(f, mainPos, SEEK_SET) != 0)
    {
        return runtimeError("Could not move to main in resolve.bin.", f);
    }
 
    string line;
    string words[MAX_WORDS];
 
    if (readRecord(f, line) < 0)
    {
        return runtimeError("Could not read the main record in resolve.bin.", f);
    }
 
    int n = splitWords(line, words, MAX_WORDS);
 
    if (n < 2 || words[0] != "func" || words[1] != "main")
    {
        return runtimeError("The first record is not 'func main'.", f);
    }
 
    // For every function call (by depth) we remember which caller variables
    // were given as arguments, so the new values can be copied back later.
    string callerNames[MAX_DEPTH][MAX_VARS];
    int callerCount[MAX_DEPTH] = {};
 
    while (!stack.isEmpty())
    {
        long long pos = readRecord(f, line);
 
        if (pos < 0)
        {
            break;
        }
 
        n = splitWords(line, words, MAX_WORDS);
 
        if (n < 0)
        {
            return runtimeError("A line has too many words (maximum is 18).", f);
        }
 
        if (n == 0)
        {
            continue;
        }
 
        string op = words[0];
 
        // ----- set name number -----
        if (op == "set")
        {
            if (n != 3)
            {
                return runtimeError("'set' needs exactly 2 values: set <name> <number>.", f);
            }
 
            int value;
 
            try
            {
                size_t used = 0;
                value = stoi(words[2], &used);
 
                if (used != words[2].size())
                {
                    throw invalid_argument("bad number");
                }
            }
            catch (...)
            {
                return runtimeError("'set' value is not a valid whole number.", f);
            }
 
            Frame& frame = stack.peek();
            Variable* var = findVariable(frame, words[1]);
 
            if (var != nullptr)
            {
                var->value = value;
            }
            else
            {
                if (frame.localCount >= MAX_VARS)
                {
                    return runtimeError("Too many local variables in one function (maximum is 16).", f);
                }
 
                frame.locals[frame.localCount].name = words[1];
                frame.locals[frame.localCount].value = value;
                frame.localCount++;
            }
 
            takeSnapshot(stack, timeline);
        }
 
        // ----- add / sub / mul / div -----
        else if (op == "add" || op == "sub" || op == "mul" || op == "div")
        {
            if (n != 3)
            {
                return runtimeError("'add/sub/mul/div' needs exactly 2 names: <op> <a> <b>.", f);
            }
 
            Frame& frame = stack.peek();
            Variable* a = findVariable(frame, words[1]);
            Variable* b = findVariable(frame, words[2]);
 
            if (a == nullptr || b == nullptr)
            {
                return runtimeError("A variable used in add/sub/mul/div does not exist.", f);
            }
 
            // use long long so a too big number can be found
            long long x = a->value;
            long long y = b->value;
            long long result = 0;
 
            if (op == "add")
            {
                result = x + y;
            }
            else if (op == "sub")
            {
                result = x - y;
            }
            else if (op == "mul")
            {
                result = x * y;
            }
            else
            {
                if (y == 0)
                {
                    return runtimeError("Division by zero.", f);
                }
 
                result = x / y;
            }
 
            if (result < -2147483647LL - 1 || result > 2147483647LL)
            {
                return runtimeError("Number is too big (overflow).", f);
            }
 
            a->value = (int)result;
 
            takeSnapshot(stack, timeline);
        }
 
        // ----- call function arguments... -----
        else if (op == "call")
        {
            if (n < 2)
            {
                return runtimeError("'call' needs a function name.", f);
            }
 
            int argCount = n - 2;
 
            if (argCount > MAX_VARS)
            {
                return runtimeError("Too many arguments in 'call' (maximum is 16).", f);
            }
 
            if (stack.size() >= MAX_DEPTH)
            {
                return runtimeError("Call stack is too deep (maximum is 64).", f);
            }
 
            // the next line after this call is where we come back to
            long long returnPos = ftell(f);
 
            if (returnPos < 0)
            {
                return runtimeError("Could not read the file position for 'call'.", f);
            }
 
            // "pos" is the address of the function (put there in Pass 1)
            if (fseek(f, pos, SEEK_SET) != 0)
            {
                return runtimeError("Could not jump to the called function.", f);
            }
 
            string funcLine;
 
            if (readRecord(f, funcLine) < 0)
            {
                return runtimeError("Could not read the called function record.", f);
            }
 
            string funcWords[MAX_WORDS];
            int funcWordCount = splitWords(funcLine, funcWords, MAX_WORDS);
 
            if (funcWordCount < 2 || funcWords[0] != "func")
            {
                return runtimeError("The call target is not a valid 'func' line.", f);
            }
 
            if (funcWordCount - 2 != argCount)
            {
                return runtimeError("Number of arguments in 'call' does not match the function.", f);
            }
 
            Frame newFrame;
            newFrame.funcName = funcWords[1];
            newFrame.argCount = argCount;
            newFrame.returnPos = (int)returnPos;
 
            Frame& caller = stack.peek();
            int depth = stack.size();
            callerCount[depth] = argCount;
 
            // give the values of the caller variables to the new function
            for (int i = 0; i < argCount; i++)
            {
                Variable* var = findVariable(caller, words[i + 2]);
 
                if (var == nullptr)
                {
                    return runtimeError("A variable passed in 'call' does not exist.", f);
                }
 
                newFrame.args[i].name = funcWords[i + 2];
                newFrame.args[i].value = var->value;
                callerNames[depth][i] = words[i + 2];
            }
 
            stack.push(newFrame);
            takeSnapshot(stack, timeline);
 
            // the file is now right after "func ...", so the next line
            // we read is the first line inside the function
        }
 
        // ----- func_end -----
        else if (op == "func_end")
        {
            takeSnapshot(stack, timeline);
 
            // if main ends, the program is finished
            if (stack.size() == 1)
            {
                stack.pop();
                break;
            }
 
            Frame finished = stack.peek();
            int depth = stack.size() - 1;
            stack.pop();
 
            Frame& caller = stack.peek();
 
            // copy the new argument values back to the caller variables
            for (int i = 0; i < callerCount[depth]; i++)
            {
                Variable* var = findVariable(caller, callerNames[depth][i]);
 
                if (var != nullptr)
                {
                    var->value = finished.args[i].value;
                }
            }
 
            // go back to the line after the call
            if (fseek(f, finished.returnPos, SEEK_SET) != 0)
            {
                return runtimeError("Could not return to the caller.", f);
            }
        }
 
        // ----- func (only the start line of a function, nothing to do) -----
        else if (op == "func")
        {
            continue;
        }
 
        else
        {
            return runtimeError("Unknown instruction.", f);
        }
    }
 
    fclose(f);
    return true;
}
 
 
// ---------------------------------------------------------------
// Pass 3 : save session.tdbg
//
// File layout:
//   [header][snapshot 0][snapshot 1] ... [index]
//
// Header (20 bytes):
//   "TTDB" (4) | version (4) | number of snapshots (4) | index position (8)
//
// One snapshot:
//   [stack depth]
//   for each frame (top of the stack first):
//     [function name] [arg count] {[arg name][arg value]}
//     [return position] [local count] {[local name][local value]}
//
// Index: for every snapshot, 8 bytes with the place where it starts.
// ---------------------------------------------------------------
 
bool writeHeader(FILE* f, int steps, long long indexPos)
{
    int version = 1;
 
    return writeBytes(f, "TTDB", 4)
        && writeInt(f, version)
        && writeInt(f, steps)
        && writeBytes(f, &indexPos, sizeof(indexPos));
}
 
bool writeVariable(FILE* f, const Variable& var)
{
    return writeText(f, var.name) && writeInt(f, var.value);
}
 
bool writeFrame(FILE* f, const Frame& frame)
{
    if (!writeText(f, frame.funcName) || !writeInt(f, frame.argCount))
    {
        return false;
    }
 
    for (int i = 0; i < frame.argCount; i++)
    {
        if (!writeVariable(f, frame.args[i]))
        {
            return false;
        }
    }
 
    if (!writeInt(f, frame.returnPos) || !writeInt(f, frame.localCount))
    {
        return false;
    }
 
    for (int i = 0; i < frame.localCount; i++)
    {
        if (!writeVariable(f, frame.locals[i]))
        {
            return false;
        }
    }
 
    return true;
}
 
bool writeSnapshot(FILE* f, const Snapshot* snap)
{
    if (!writeInt(f, snap->depth))
    {
        return false;
    }
 
    for (int i = 0; i < snap->depth; i++)
    {
        if (!writeFrame(f, snap->frames[i]))
        {
            return false;
        }
    }
 
    return true;
}
 
bool saveSession(Timeline& timeline, const char* fileName)
{
    FILE* f = fopen(fileName, "wb+");
 
    if (f == nullptr)
    {
        return false;
    }
 
    int total = timeline.size();
 
    // the header is written again at the end, when the index position is known
    if (!writeHeader(f, total, 0))
    {
        fclose(f);
        return false;
    }
 
    // the index remembers where each snapshot starts
    long long* index = new long long[total + 1];
 
    // write all snapshots
    TimeNode* cur = timeline.first();
    int i = 0;
 
    while (cur != nullptr && i < total)
    {
        long pos = ftell(f);
 
        if (pos < 0)
        {
            delete[] index;
            fclose(f);
            return false;
        }
 
        index[i] = pos;
 
        if (!writeSnapshot(f, cur->snap))
        {
            delete[] index;
            fclose(f);
            return false;
        }
 
        cur = cur->next;
        i++;
    }
 
    // write the index
    long indexPos = ftell(f);
 
    if (indexPos < 0)
    {
        delete[] index;
        fclose(f);
        return false;
    }
 
    for (int k = 0; k < total; k++)
    {
        if (!writeBytes(f, &index[k], sizeof(long long)))
        {
            delete[] index;
            fclose(f);
            return false;
        }
    }
 
    // go back and write the header again with the real index position
    if (fseek(f, 0, SEEK_SET) != 0 || !writeHeader(f, total, indexPos))
    {
        delete[] index;
        fclose(f);
        return false;
    }
 
    delete[] index;
    fclose(f);
    return true;
}
 
 
// ---------------------------------------------------------------
// main
// ---------------------------------------------------------------
 
int main()
{
    cout << "STEP 1: Starting Time-Travel Debugger..." << endl;
 
    // Pass 0
    if (!validateProgram("source.bin"))
    {
        cout << "ERROR: Validation failed." << endl;
        return 1;
    }
 
    cout << "STEP 2: Validation passed." << endl;
 
    // Pass 1
    long long mainPos = resolveProgram("source.bin", "resolve.bin");
 
    if (mainPos < 0)
    {
        cout << "ERROR: Resolve failed." << endl;
        return 1;
    }
 
    cout << "STEP 3: Resolve passed. main offset = " << mainPos << endl;
 
    // Pass 2
    Timeline timeline;
    bool ok = executeProgram("resolve.bin", mainPos, timeline);
 
    if (!ok)
    {
        cout << "WARNING: Execution stopped early because of an error. "
             << "Snapshots taken before the error are still saved." << endl;
    }
 
    cout << "STEP 4: Execution finished. Snapshots = " << timeline.size() << endl;
 
    // Pass 3
    if (!saveSession(timeline, "session.tdbg"))
    {
        cout << "ERROR: Could not write session.tdbg." << endl;
        return 1;
    }
 
    cout << "STEP 5: session.tdbg created." << endl;
 
    if (!ok)
    {
        return 2;
    }
 
    return 0;
}
