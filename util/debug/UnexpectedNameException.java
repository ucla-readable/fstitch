public class UnexpectedNameException extends BadInputException
{
	private final String name;
	
	public UnexpectedNameException(String name)
	{
		super("Unexpected name: " + name);
		this.name = name;
	}
	
	public String getName()
	{
		return name;
	}
}
