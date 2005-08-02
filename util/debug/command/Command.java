/**
 * The interface supported by all commands.
 *
 * All commands should implement this interface.
 * */

package command;

public interface Command
{
	/**
	 * Get the name of the command.
	 *
	 * The name of the command is the string that should be typed to run it.
	 *
	 * @return The name of the command.
	 * */
	public String getName();
	
	/**
	 * Get help for the command.
	 *
	 * The help text should be a short description of the operation or usage
	 * of the command.
	 *
	 * @return The help for the command.
	 * */
	public String getHelp();
	
	/**
	 * Run the command.
	 *
	 * Runs the command on the given arguments, with the given state data.
	 * The state data can be used to pass any object to the command, like
	 * program data structures to be used or modified.
	 *
	 * The return value of the command can likewise be used to return any
	 * state data desired.
	 *
	 * @param args The arguments to the command, from the command line.
	 * @param data The (optional) state data for the command.
	 * @param interpreter The CommandInterpreter running the command.
	 * @return The (optional) returned state data.
	 * */
	public Object runCommand(String[] args, Object data, CommandInterpreter interpreter) throws CommandException;
}
