import sys
from threading import Thread, Event
from queue import Queue
from time import sleep
from transformers import StoppingCriteriaList
from lemonade.tools.server.serve import StopOnEvent


employee_handbook = """
1. You will work very hard every day.\n
2. You are allowed to listen to music, but must wear headphones.\n
3. Remember, the break room fridge is not a science experiment. 
    Please label and remove your leftovers regularly!\n
"""


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


def plain_text_search(
    question: str, streamer: TextStreamer, stopping_criteria: StoppingCriteriaList
):
    """
    Searches the employee handbook, looking for an exact match and
    returns an answer if available.

    Imitates an LLM's generate function by streaming text to a queue.

    Not needed once we integrate with the lemonade API.
    """

    # Turn the question into key words
    # Remove punctuation and convert to lower-case
    sanitized_question = question.replace("?", "").replace(".", "").lower()
    # Get a list of important words (longer than length 3)
    keywords = [word for word in sanitized_question.split(" ") if len(word) > 3]

    # Search for the key words in the employee handbook
    result = None
    for keyword in keywords:
        for line in employee_handbook.lower().split("\n"):
            if keyword in line:
                result = line

    if result:
        response = (
            f"This line of the employee handbook might be relevant to you: {result}"
        )
    else:
        response = (
            "I am sorry, I didn't find anything that is useful to you. Please "
            "try again with another question or read the entire employee handbook "
            "cover-to-cover to make sure that you didn't miss any rules."
        )

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
            "question": user_message,
            "streamer": streamer,
            "stopping_criteria": stopping_criteria,
        }

        thread = Thread(target=plain_text_search, kwargs=generation_kwargs)
        thread.start()

        # Print each word to the screen as it arrives from the streamer
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
