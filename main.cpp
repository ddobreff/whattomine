#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jsoncpp/json/json.h>

// Sample config file

//{
//  "coins": {
//    "Ethereum": {
//      "fee":0.01,
//      "cmd":"/usr/local/bin/ethminer -U -R --display-interval 30 --noeval -P stratum+tls12://0x1234...6789.miner0@us1.ethermine.org:5555"
//     },
//     "EthereumClassic": {
//       "fee":0.01,
//       "cmd":"/usr/local/bin/ethminer -U -R --display-interval 30 --noeval -P stratum+tls12://0x1234...6789839.miner0@us1-etc.ethermine.org:5555"
//     },
//     "Dubaicoin": {
//       "fee":0.0015,
//       "cmd":"/usr/local/bin/ethminer -U -R --display-interval 30 --noeval -P stratum://0x1234...6789.miner0@mine.house:7007"
//     }
//  }
//}

#define WTM_DEBUG 0

#define DBGSTR(_s)              \
    do                          \
    {                           \
        if (WTM_DEBUG)          \
            clog << _s << '\n'; \
    } while (false)

using namespace std;

static double revenue = 0.0;

Json::Value coinroot;

static bool FindCoin(string coin)
{
    return (*coinroot.begin()).isMember(coin);
}

// Query whattomine.com for the most profitable coin
static string FindBestCoin(std::stringstream& coinlist)
{
    DBGSTR("FindBestCoin called");
    try
    {
        // Retrieve the list of coins as json formatted file
        DBGSTR("FindBestCoin retreiving JSON");
        system("wget -q4O coins.json https://whattomine.com/coins.json");
        DBGSTR("FindBestCoin parsing JSON");
        ifstream ifs("coins.json");
        Json::Reader reader;
        Json::Value root;
        // Parse the json contents
        if (!reader.parse(ifs, root))
            return "error";
        ifs.close();
        remove("coins.json");
        DBGSTR("FindBestCoin JSON parsed");

        string bestcoin = "error";
        revenue = 0.0;

        // Iterate the coin types
        for (Json::Value::const_iterator outer = root.begin(); outer != root.end(); outer++)
        {
            for (Json::Value::const_iterator inner = (*outer).begin(); inner != (*outer).end();
                 inner++)
            {
                string coin = inner.key().asString();
                if (FindCoin(coin))
                {
                    float rate = 1.0 - (*coinroot.begin())[coin]["fee"].asFloat();
                    double rev = atof((*inner)["btc_revenue"].asString().c_str()) * rate;
                    coinlist << setw(20) << left << coin << " " << rev << '\n';
                    // Save the most profitable
                    if (rev > revenue)
                    {
                        revenue = rev;
                        bestcoin = coin;
                    }
                }
            }
        }
        // return the best choice
        return bestcoin;
    }
    catch (...)
    {
        // Something went wrong!!!
        DBGSTR("FindBestCoin returning error");
        return "error";
    }
    DBGSTR("FindBestCoin returning");
    return 0;
}

static vector<string> split(const char* str)
{
    char* s = strdup(str);
    vector<string> result;
    char* token = strtok(s, " ");
    while (token != NULL)
    {
        result.push_back(token);
        token = strtok(NULL, " ");
    }
    free(s);
    return result;
}

#define SECONDS(s) (s)
#define MINUTES(m) (m * SECONDS(60u))
#define POLL_INTERVAL SECONDS(10u)
#define WTM_INTERVAL (30u * POLL_INTERVAL)

static pid_t pid = 0;
static unsigned poll_seconds = 0;
static unsigned wtm_seconds = 0;

// Launch the miner in a separate process
static void Launch(const char* argv, stringstream& coinlist)
{
    auto cmd = split(argv);
    char** args = (char**)calloc(cmd.size() + 1, sizeof(char*));
    coinlist << "Command: ";
    for (unsigned i = 0; i < cmd.size(); i++)
    {
        args[i] = (char*)cmd[i].c_str();
        coinlist << cmd[i] << ' ';
    }
    coinlist << '\n';

    args[cmd.size()] = NULL;

    /*Spawn a child to run the program.*/
    pid = fork();
    if (pid == 0)
    { /* child process */
        execv(args[0], args);
        exit(-1); /* only if execv fails */
    }
    free(args);
}

bool LoadConfig()
{
    ifstream ifs(".wtm.conf");
    if (!ifs.is_open())
        return false;
    Json::Reader reader;
    // Parse the json contents
    if (!reader.parse(ifs, coinroot))
        return false;
    ifs.close();
    cout << "Enabled coins:";
    Json::Value::const_iterator outer = coinroot.begin();
    for (Json::Value::const_iterator inner = (*outer).begin(); inner != (*outer).end(); inner++)
        cout << ' ' << inner.key().asString();
    cout << '\n';
    // cout << coinroot << '\n';
    return true;
}

// Check whattomine every minute for most profitable coin
int main(int ac, char* av[])
{
    if (!LoadConfig())
    {
        cerr << "Can't parse .wtm.conf\n";
        exit(-1);
    }
    string lastcoin;
    double lastRevenue = 0.0;
    auto lasttime = chrono::system_clock::now();
    while (1)
    {
        // check to see if miner has terminated on its own
        if (pid)
        {
            int status;
            if (waitpid(pid, &status, WNOHANG))
            {
                // miner has self terminated
                lastcoin = "";
                pid = 0;
                poll_seconds = 0;
                wtm_seconds = 0;
                lastRevenue = 0;
            }
        }

        if (poll_seconds > POLL_INTERVAL)
            poll_seconds -= POLL_INTERVAL;
        else
        {
            poll_seconds = POLL_INTERVAL;
            if (wtm_seconds > POLL_INTERVAL)
                wtm_seconds -= POLL_INTERVAL;
            else
            {
                wtm_seconds = WTM_INTERVAL;
                stringstream coinlist;
                // Get the best coin choice from whattomine
                string bestcoin = FindBestCoin(coinlist);
                // Ignore errors and check to see if a better choice has been found
                if ((bestcoin != "error") && (bestcoin != lastcoin) && (revenue != lastRevenue))
                {
                    lastRevenue = revenue;

                    std::ofstream file;
                    file.open("whatlog.txt", std::ios::out | std::ios::app);

                    // Kill current miner
                    if (pid)
                    {
                        kill(pid, SIGINT);
                        waitpid(pid, 0, 0);
                        sleep(1);
                    }

                    auto now = chrono::system_clock::now();
                    auto mins = std::chrono::duration_cast<std::chrono::seconds>(now - lasttime);
                    lasttime = now;

                    coinlist << "Runtime: " << mins.count() / 60.0 << " minutes\n===\n";

                    lastcoin = bestcoin;

                    auto timenow = chrono::system_clock::to_time_t(now);
                    char* ct = ctime(&timenow);
                    ct[strlen(ct) - 1] = 0;

                    coinlist << "Switching to: " << bestcoin << ", " << revenue << ", " << ct
                             << '\n';

                    // Start mining new best coin
                    Launch((*coinroot.begin())[bestcoin]["cmd"].asString().c_str(), coinlist);

                    cout << coinlist.str();
                    file << coinlist.str();

                    file.close();
                }
            }
        }

        sleep(POLL_INTERVAL);
    }
    return 0;
}
