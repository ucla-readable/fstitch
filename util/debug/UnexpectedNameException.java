public class UnexpectedNameException extends BadInputException
{
	public final String name;
	
	public UnexpectedNameException(String name, int offset)
	{
		super("Unexpected name: " + name, offset);
		this.name = name;
	}
}
