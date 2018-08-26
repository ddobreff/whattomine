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

using namespace std;

// Common part of the miner command

#define EXECUTABLE "/usr/local/bin/ethminer"
#define GPU_COMMON " -R --display-interval 30 --noeval"

static const char* const commonCmd =
#if defined(AMD)
    EXECUTABLE GPU_COMMON " -G";
#define MINER "miner1"
#else
    EXECUTABLE GPU_COMMON " -U --cuda-streams 2 --cuda-block-size 64";
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
    {"Ethereum", {.01, .000,
                     "-P stratum+tls12://" WALLET(ETHWALLET) "." MINER
                     "@us1.ethermine.org:5555"}},
    {"EthereumClassic", {.01, .000,
                            "-P stratum+tls12://" WALLET(ETCWALLET) "." MINER
                            "@us1-etc.ethermine.org:5555"}}};

static double revenue = 0.0;

// Query whattomine.com for the most profitable coin
static string FindBestCoin(std::stringstream& coinlist)
{
    try
    {
        // Retrieve the list of coins as json formatted file
        system("wget -q -O coins.json https://whattomine.com/coins.json");
        ifstream ifs("coins.json");
        Json::Reader reader;
        Json::Value root;
        // Parse the json contents
        if (!reader.parse(ifs, root))
            return "error";
        ifs.close();
        remove("coins.json");

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
        return "error";
    }
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

static pid_t pid = 0;
static unsigned minutes = 0;

#define POLLMINUTES 2

// Launch the miner in a separate process
static void Launch(const char* argv, stringstream& coinlist)
{
    auto common = split(commonCmd);
    auto pools = split(argv);
    common.insert(common.end(), pools.begin(), pools.end());
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
    // Stay on coin at least 2 minutes
    minutes = 3;
}

// Check whattomine every minute for most profitable coin
int main()
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
                minutes = 0;
            }
        }

        if (minutes > POLLMINUTES)
            minutes -= POLLMINUTES;
        else
        {
            stringstream coinlist;
            minutes = 0;
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

                coinlist << "Switching to: " << bestcoin << ", " << revenue << ", " << ct << '\n';

                // Start mining new best coin
                Launch(pools[bestcoin].cmd, coinlist);

                cout << coinlist.str();
                file << coinlist.str();

                file.close();
            }
        }

        sleep(POLLMINUTES * 60);
    }
    return 0;
}

