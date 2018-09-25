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

#define WTM_DEBUG 0

#define DBGSTR(_s)              \
    do                          \
    {                           \
        if (WTM_DEBUG)          \
            clog << _s << '\n'; \
    } while (false)

using namespace std;

// Common part of the miner command

#define EXECUTABLE "/usr/local/bin/ethminer"
#define GPU_COMMON " -R --display-interval 30 --noeval"

static const char* const commonCmd =
#if defined(AMD)
    EXECUTABLE GPU_COMMON " -G";
#define MINER "miner1"
#else
    EXECUTABLE GPU_COMMON " -U";
#define MINER "miner0"
#endif

#define WALLET(x) W2(x)
#define W2(x) #x

typedef struct
{
    double poolfee;
    double exchfee;
    const char* cmd;
} pool_t;

// Supported coins (ethash)
static map<string, pool_t> pools = {
    // coin,(pool fee, exchange fee, pool)
    {"Ethereum",
        {.01, .000, "-P stratum+tls12://" WALLET(ETHWALLET) "." MINER "@us1.ethermine.org:5555"}},
    {"EthereumClassic",
        {.01, .000,
            "-P stratum+tls12://" WALLET(ETCWALLET) "." MINER "@us1-etc.ethermine.org:5555"}}
    //    ,{"Metaverse",
    //        {.001, .001,
    //            "-P stratum://" WALLET(ETPWALLET) "." MINER "@etp.sandpool.org:8009"}}
};

static double revenue = 0.0;

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
                for (Json::Value::const_iterator inner2 = (*inner).begin();
                     inner2 != (*inner).end(); inner2++)
                {
                    // Only look at Ethash coins
                    if (inner2.key() == "algorithm" && *inner2 == "Ethash")
                    {
                        string coin = inner.key().asString();
                        // Filter the coin types we want to mine
                        auto it = pools.find(coin);
                        if (it != pools.end())
                        {
                            double rate = (1.0 - it->second.poolfee) * (1.0 - it->second.exchfee);
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
static void Launch(const char* argv, stringstream& coinlist, int ac, char* av[])
{
    auto common = split(commonCmd);
    auto pools = split(argv);
    common.insert(common.end(), pools.begin(), pools.end());
    for (int i = 1; i < ac; i++)
        common.push_back(string(av[i]));
    char** args = (char**)calloc(common.size() + 1, sizeof(char*));
    coinlist << "Command: ";
    for (unsigned i = 0; i < common.size(); i++)
    {
        args[i] = (char*)common[i].c_str();
        coinlist << common[i] << ' ';
    }
    coinlist << '\n';

    args[common.size()] = NULL;

    /*Spawn a child to run the program.*/
    pid = fork();
    if (pid == 0)
    { /* child process */
        execv(args[0], args);
        exit(-1); /* only if execv fails */
    }
    free(args);
}

// Check whattomine every minute for most profitable coin
int main(int ac, char* av[])
{
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
                    Launch(pools[bestcoin].cmd, coinlist, ac, av);

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
