/**
 * A default quit command.
 *
 * This command sets the quit flag to true when 'quit' is typed.
 * */

package command;

public class QuitCommand implements Command
{
	private String name, help;
	
	/**
	 * Default constructor.
	 * */
	public QuitCommand()
	{
		this("Quits the program.");
	}
	
	/**
	 * Constructor taking the help text only.
	 * */
	public QuitCommand(String h)
	{
		this("quit", h);
	}
	
	/**
	 * Constructor taking the name of the command and its help text.
	 * */
	public QuitCommand(String n, String h)
	{
		name = n;
		help = h;
	}
	
	public String getName()
	{
		return name;
	}
	
	public String getHelp()
	{
		return help;
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		interpreter.setQuitFlag(true);
		return data;
	}
}
