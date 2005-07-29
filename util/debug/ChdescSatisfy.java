import java.io.DataInput;
import java.io.IOException;

public class ChdescSatisfy extends Opcode
{
	private final int chdesc;
	
	public ChdescSatisfy(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SATISFY, "KDB_CHDESC_SATISFY", ChdescSatisfy.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
