import sys
from threading import Thread, Event
from queue import Queue
from time import sleep
from transformers import StoppingCriteriaList
from lemonade.tools.server.serve import StopOnEvent


class TextStreamer:
    """
    Imitates a queue for streaming text from one thread to another.

    Not needed once we integrate with the lemonade API.
    """

    def __init__(self):
        self.text_queue = Queue()
        self.stop_signal = None

    def add_text(self, text: str):
        self.text_queue.put(text)

    def done(self):
        self.text_queue.put(self.stop_signal)

    def __iter__(self):
        return self

    def __next__(self):
        value = self.text_queue.get()
        if value == self.stop_signal:
            raise StopIteration()
        else:
            return value


def generate_placeholder(
    streamer: TextStreamer, stopping_criteria: StoppingCriteriaList
):
    """
    Imitates an LLM's generate function by streaming text to a queue.

    Not needed once we integrate with the lemonade API.
    """

    # pylint: disable=line-too-long
    response = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum."

    for word in response.split(" "):
        streamer.add_text(f"{word} ")
        sleep(0.05)

        if stopping_criteria[0].stop_event.is_set():
            break

    streamer.done()


def main():

    while True:
        # Enable sending a signal into the generator thread to stop
        # the generation early
        stop_event = Event()
        stopping_criteria = StoppingCriteriaList([StopOnEvent(stop_event)])

        # Prompt the user for an input message
        print()
        user_message = input("User: ")
        print()

        # Print a friendly message when we quit
        if user_message == "quit":
            print("System: Ok, bye!\n")
            break

        # Generate the response in a thread and stream the result back
        # to the main thread
        streamer = TextStreamer()
        generation_kwargs = {
            "streamer": streamer,
            "stopping_criteria": stopping_criteria,
        }

        thread = Thread(target=generate_placeholder, kwargs=generation_kwargs)
        thread.start()

        # Print each word to the screen as it arrives
        # Allow the user to terminate the response with
        # a keyboard interrupt (ctrl+c)
        try:
            print("LLM: ", end="")
            for new_text in streamer:
                print(new_text, end="")
                sys.stdout.flush()

        except KeyboardInterrupt:
            stop_event.set()

        print()

        thread.join()


if __name__ == "__main__":
    main()
