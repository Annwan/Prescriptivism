#ifndef PRESCRIPTIVISM_CLIENT_CLIENT_HH
#define PRESCRIPTIVISM_CLIENT_CLIENT_HH

#include <Client/Render/GL.hh>
#include <Client/Render/Render.hh>
#include <Client/UI/UI.hh>

#include <Shared/Cards.hh>
#include <Shared/Constants.hh>
#include <Shared/Packets.hh>
#include <Shared/TCP.hh>
#include <Shared/Utils.hh>

#include <base/Base.hh>

#include <functional>
#include <generator>
#include <memory>
#include <ranges>
#include <thread>
#include <vector>

namespace pr::client {
class Client;
class MenuScreen;
class ErrorScreen;
class ConnexionScreen;
class WaitingScreen;
class WordChoiceScreen;
class GameScreen;
class Player;
} // namespace pr::client

// =============================================================================
//  General Screens
// =============================================================================
/// Main menu of the game.
class pr::client::MenuScreen : public Screen {
public:
    MenuScreen(Client& c);
};

/// Screen that displays an error.
class pr::client::ErrorScreen : public Screen {
    /// Text to display.
    Label* msg;

    /// The screen to return to after this one is closed.
    Screen* return_screen = nullptr;

public:
    ErrorScreen(Client& c);

    /// Set the text to display and the screen to return to.
    void enter(Client& c, std::string t, Screen& return_to);
};

// =============================================================================
//  Connexion Phase Screens
// =============================================================================
/// Screen that is displayed while we connect to a server.
///
/// This screen has all the connexion logic.
class pr::client::ConnexionScreen : public Screen {
    enum class State : u32 {
        Entered,    ///< The screen has just been entered.
        Connecting, ///< We are trying to connect in a separate thread.
        Aborted,    ///< The 'Cancel' button was pressed.
    };

    Client& client;
    State st;
    std::string address, username, password;
    Thread<net::TCPConnexion> connexion_thread;

public:
    ConnexionScreen(Client& c);

    void enter(std::string address, std::string username, std::string password);
    void tick(InputSystem& input) override;
    void set_address(std::string addr);

private:
    auto connexion_thread_main(std::string address, std::stop_token st) -> Result<net::TCPConnexion>;
};

class pr::client::WaitingScreen : public Screen {
public:
    WaitingScreen(Client& c);
};

class pr::client::WordChoiceScreen : public Screen {
    Client& client;
    CardStacks* cards;
    constants::Word original_word;
    CardStacks::Stack* selected = nullptr;

    void SendWord();

public:
    WordChoiceScreen(Client& c);

    // This takes an array instead of a span to force a specific word size.
    void enter(const constants::Word& word);
    void on_refresh(Renderer& r) override;
    void tick(InputSystem& input) override;
};

// =============================================================================
//  In-game Screens
// =============================================================================
class pr::client::Player {
    LIBBASE_MOVE_ONLY(Player);

    /// The server-side player id.
    Readonly(u8, id);

    /// The player name.
    Readonly(std::string, name);

public:
    /// The current word of this player.
    CardStacks* word{};

    explicit Player() = default;
    explicit Player(std::string name, u8 id) : _id{id}, _name{std::move(name)} {}
};

/// This screen renders the actual game.
class pr::client::GameScreen : public Screen {
    struct Validator;
    friend Validator;

    enum struct State {
        /// The starting state. Nothing is selected.
        NoSelection,

        /// It is not our turn. User interaction is passed.
        NotOurTurn,

        /// A card in hand is selected, and we are waiting for
        /// the user to select a target for it.
        ///
        /// 'our_selected_card' holds the selected sound card.
        SingleTarget,

        /// We pressed the pass button; prompt the user to select
        /// a card to discard.
        Passing,
    };

    /// A targeted card in someone’s word.
    struct Target {
        CardStacks::Stack* stack;
        std::optional<u32> card_idx;

        explicit Target(CardStacks::Stack& s) : stack{&s} {}
    };

    Client& client;

    /// The end turn / pass / cancel button in the lower
    /// right corner of the screen.
    Button* end_turn_button;

    /// The other players in the game.
    std::vector<Player> other_players;

    /// All players, including us.
    std::vector<Player*> all_players;

    /// Our player object.
    Player us;

    /// The cards in our hand.
    CardStacks* our_hand{};

    /// The words of other players.
    Group* other_words{};

    /// The card widget used to preview a card.
    Card* preview{};

    /// The last card that was selected by the player
    Card* our_selected_card{};

    /// The current game state.
    State state = State::NotOurTurn;

public:
    explicit GameScreen(Client& c);
    void add_card(PlayerId id, u32 stack_idx, CardId card);
    void add_card_to_hand(CardId id);
    void enter(packets::sc::StartGame sg);
    void end_turn();
    void lock_changed(PlayerId player, u32 stack_index, bool locked);
    void on_refresh(Renderer& r) override;
    void start_turn();
    void tick(InputSystem& input) override;

private:
    void ClearSelection(State new_state = State::NoSelection);
    void Discard(CardStacks::Stack& stack);
    void Pass();
    auto PlayerById(PlayerId id) -> Player&;
    auto PlayerForCardInWord(Card* c) -> Player*; /// Return the player that owns this card in their word.
    void ResetHand();
    void ResetWords(Selectable s = Selectable::No, Card::Overlay o = Card::Overlay::Default);

    /// Get all valid targets for the card at this index, assuming it is in our hand.
    auto Targets(Card& c) -> std::generator<Target>;

    void TickNoSelection();
    void TickNotOurTurn();
    void TickPassing();
    void TickSingleTarget();
    auto ValidatorFor(Player& p) -> Validator;
};

// =============================================================================
//  Client
// =============================================================================
class pr::client::Client {
    LIBBASE_IMMOVABLE(Client);

    struct InitRenderer {
        InitRenderer(Renderer& r) { Renderer::SetThreadRenderer(r); }
    };

public:
    /// The main renderer.
    Renderer renderer;
    InitRenderer _{renderer};

    /// The user input handler.
    InputSystem input_system{renderer};

    /// Screens.
    MenuScreen menu_screen{*this};
    ConnexionScreen connexion_screen{*this};
    ErrorScreen error_screen{*this};
    WaitingScreen waiting_screen{*this};
    WordChoiceScreen word_choice_screen{*this};
    GameScreen game_screen{*this};

    /// Connexion to the game server.
    net::TCPConnexion server_connexion;

private:
    Screen* current_screen = nullptr;

    explicit Client(Renderer r);

public:
    /// Run the client for ever.
    static void Run();

    /// Run the client for ever, and immediately connect to a server.
    static void RunAndConnect(
        std::string address,
        std::string username,
        std::string password
    );

    /// Enter a screen.
    void enter_screen(Screen& s);

    /// Show an error to the user.
    void show_error(std::string error, Screen& return_to);

#define X(name) void handle(packets::sc::name);
    COMMON_PACKETS(X)
    SC_PACKETS(X)
#undef X

private:
    static auto Startup() -> Renderer;

    void RunGame();
    void Tick();
    void TickNetworking();
};

#endif // PRESCRIPTIVISM_CLIENT_CLIENT_HH
